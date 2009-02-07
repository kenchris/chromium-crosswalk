// Copyright (c) 2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/ipc_channel_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if defined(OS_LINUX)
#include <linux/un.h>
#elif defined(OS_MACOSX)
#include <sys/un.h>
#endif

#include <string>
#include <map>

#include "base/command_line.h"
#include "base/lock.h"
#include "base/logging.h"
#include "base/process_util.h"
#include "base/scoped_ptr.h"
#include "base/string_util.h"
#include "base/singleton.h"
#include "chrome/common/chrome_counters.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/file_descriptor_posix.h"
#include "chrome/common/ipc_message_utils.h"

namespace IPC {

//------------------------------------------------------------------------------
namespace {

// When running as a browser, we install the client socket in a specific file
// descriptor number (@kClientChannelFd). However, we also have to support the
// case where we are running unittests in the same process.
//
// We do not support forking without execing.
//
// Case 1: normal running
//   The IPC server object will install a mapping in PipeMap from the
//   name which it was given to the client pipe. When forking the client, the
//   GetClientFileDescriptorMapping will ensure that the socket is installed in
//   the magic slot (@kClientChannelFd). The client will search for the
//   mapping, but it won't find any since we are in a new process. Thus the
//   magic fd number is returned. Once the client connects, the server will
//   close it's copy of the client socket and remove the mapping.
//
// Case 2: unittests - client and server in the same process
//   The IPC server will install a mapping as before. The client will search
//   for a mapping and find out. It duplicates the file descriptor and
//   connects. Once the client connects, the server will close the original
//   copy of the client socket and remove the mapping. Thus, when the client
//   object closes, it will close the only remaining copy of the client socket
//   in the fd table and the server will see EOF on its side.
//
// TODO(port): a client process cannot connect to multiple IPC channels with
// this scheme.

class PipeMap {
 public:
  // Lookup a given channel id. Return -1 if not found.
  int Lookup(const std::string& channel_id) {
    AutoLock locked(lock_);

    ChannelToFDMap::const_iterator i = map_.find(channel_id);
    if (i == map_.end())
      return -1;
    return i->second;
  }

  // Remove the mapping for the given channel id. No error is signaled if the
  // channel_id doesn't exist
  void Remove(const std::string& channel_id) {
    AutoLock locked(lock_);

    ChannelToFDMap::iterator i = map_.find(channel_id);
    if (i != map_.end())
      map_.erase(i);
  }

  // Insert a mapping from @channel_id to @fd. It's a fatal error to insert a
  // mapping if one already exists for the given channel_id
  void Insert(const std::string& channel_id, int fd) {
    AutoLock locked(lock_);
    DCHECK(fd != -1);

    ChannelToFDMap::const_iterator i = map_.find(channel_id);
    CHECK(i == map_.end()) << "Creating second IPC server for '"
                           << channel_id
                           << "' while first still exists";
    map_[channel_id] = fd;
  }

 private:
  Lock lock_;
  typedef std::map<std::string, int> ChannelToFDMap;
  ChannelToFDMap map_;
};

// This is the file descriptor number that a client process expects to find its
// IPC socket.
static const int kClientChannelFd = 3;

// Used to map a channel name to the equivalent FD # in the client process.
int ChannelNameToClientFD(const std::string& channel_id) {
  // See the large block comment above PipeMap for the reasoning here.
  const int fd = Singleton<PipeMap>()->Lookup(channel_id);
  if (fd != -1)
    return dup(fd);

  // If we don't find an entry, we assume that the correct value has been
  // inserted in the magic slot.
  return kClientChannelFd;
}

//------------------------------------------------------------------------------
// The -1 is to take the NULL terminator into account.
#if defined(OS_LINUX)
const size_t kMaxPipeNameLength = UNIX_PATH_MAX - 1;
#elif defined(OS_MACOSX)
// OS X doesn't define UNIX_PATH_MAX
// Per the size specified for the sun_path structure of sockaddr_un in sys/un.h.
const size_t kMaxPipeNameLength = 104 - 1;
#endif

// Creates a Fifo with the specified name ready to listen on.
bool CreateServerFifo(const std::string &pipe_name, int* server_listen_fd) {
  DCHECK(server_listen_fd);
  DCHECK(pipe_name.length() > 0);
  DCHECK(pipe_name.length() < kMaxPipeNameLength);

  if (pipe_name.length() == 0 || pipe_name.length() > kMaxPipeNameLength) {
    return false;
  }

  // Create socket.
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }

  // Make socket non-blocking
  if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
    close(fd);
    return false;
  }

  // Delete any old FS instances.
  unlink(pipe_name.c_str());

  // Create unix_addr structure
  struct sockaddr_un unix_addr;
  memset(&unix_addr, 0, sizeof(unix_addr));
  unix_addr.sun_family = AF_UNIX;
  snprintf(unix_addr.sun_path, kMaxPipeNameLength + 1, "%s", pipe_name.c_str());
  size_t unix_addr_len = offsetof(struct sockaddr_un, sun_path) +
      strlen(unix_addr.sun_path) + 1;

  // Bind the socket.
  if (bind(fd, reinterpret_cast<const sockaddr*>(&unix_addr),
           unix_addr_len) != 0) {
    close(fd);
    return false;
  }

  // Start listening on the socket.
  const int listen_queue_length = 1;
  if (listen(fd, listen_queue_length) != 0) {
    close(fd);
    return false;
  }

  *server_listen_fd = fd;
  return true;
}

// Accept a connection on a fifo.
bool ServerAcceptFifoConnection(int server_listen_fd, int* server_socket) {
  DCHECK(server_socket);

  int accept_fd = accept(server_listen_fd, NULL, 0);
  if (accept_fd < 0)
    return false;
  if (fcntl(accept_fd, F_SETFL, O_NONBLOCK) == -1) {
    close(accept_fd);
    return false;
  }

  *server_socket = accept_fd;
  return true;
}

bool ClientConnectToFifo(const std::string &pipe_name, int* client_socket) {
  DCHECK(client_socket);
  DCHECK(pipe_name.length() < kMaxPipeNameLength);

  // Create socket.
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG(ERROR) << "fd is invalid";
    return false;
  }

  // Make socket non-blocking
  if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
    LOG(ERROR) << "fcnt failed";
    close(fd);
    return false;
  }

  // Create server side of socket.
  struct sockaddr_un  server_unix_addr;
  memset(&server_unix_addr, 0, sizeof(server_unix_addr));
  server_unix_addr.sun_family = AF_UNIX;
  snprintf(server_unix_addr.sun_path, kMaxPipeNameLength + 1, "%s",
           pipe_name.c_str());
  size_t server_unix_addr_len = offsetof(struct sockaddr_un, sun_path) +
      strlen(server_unix_addr.sun_path) + 1;

  int ret_val = -1;
  do {
    ret_val = connect(fd, reinterpret_cast<sockaddr*>(&server_unix_addr),
                      server_unix_addr_len);
  } while (ret_val == -1 && errno == EINTR);
  if (ret_val != 0) {
    close(fd);
    return false;
  }

  *client_socket = fd;
  return true;
}

}  // namespace
//------------------------------------------------------------------------------

Channel::ChannelImpl::ChannelImpl(const std::wstring& channel_id, Mode mode,
                                  Listener* listener)
    : mode_(mode),
      is_blocked_on_write_(false),
      message_send_bytes_written_(0),
      uses_fifo_(CommandLine::ForCurrentProcess()->HasSwitch(
                     switches::kTestingChannelID)),
      server_listen_pipe_(-1),
      pipe_(-1),
      client_pipe_(-1),
      listener_(listener),
      waiting_connect_(true),
      processing_incoming_(false),
      factory_(this) {
  if (!CreatePipe(channel_id, mode)) {
    // The pipe may have been closed already.
    LOG(WARNING) << "Unable to create pipe named \"" << channel_id <<
                    "\" in " << (mode == MODE_SERVER ? "server" : "client") <<
                    " mode error(" << strerror(errno) << ").";
  }
}

const std::wstring Channel::ChannelImpl::PipeName(
    const std::wstring& channel_id) const {
  // TODO(playmobil): This should live in the Chrome user data directory.
  // TODO(playmobil): Cleanup any stale fifos.
  return L"/var/tmp/chrome_" + channel_id;
}

bool Channel::ChannelImpl::CreatePipe(const std::wstring& channel_id,
                                      Mode mode) {
  DCHECK(server_listen_pipe_ == -1 && pipe_ == -1);
  pipe_name_ = WideToUTF8(PipeName(channel_id));

  if (uses_fifo_) {
    // TODO(playmobil): Should we just change pipe_name to be a normal string
    // everywhere?

    if (mode == MODE_SERVER) {
      if (!CreateServerFifo(pipe_name_, &server_listen_pipe_)) {
        return false;
      }
    } else {
      if (!ClientConnectToFifo(pipe_name_, &pipe_)) {
        return false;
      }
      waiting_connect_ = false;
    }
  } else {
    // socketpair()
    if (mode == MODE_SERVER) {
      int pipe_fds[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_fds) != 0) {
        return false;
      }
      // Set both ends to be non-blocking.
      if (fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) == -1 ||
          fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK) == -1) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
      }
      pipe_ = pipe_fds[0];
      client_pipe_ = pipe_fds[1];

      Singleton<PipeMap>()->Insert(pipe_name_, client_pipe_);
    } else {
      pipe_ = ChannelNameToClientFD(pipe_name_);
      DCHECK(pipe_ > 0);
      waiting_connect_ = false;
    }
  }

  // Create the Hello message to be sent when Connect is called
  scoped_ptr<Message> msg(new Message(MSG_ROUTING_NONE,
                                      HELLO_MESSAGE_TYPE,
                                      IPC::Message::PRIORITY_NORMAL));
  if (!msg->WriteInt(base::GetCurrentProcId())) {
    Close();
    return false;
  }

  output_queue_.push(msg.release());
  return true;
}

bool Channel::ChannelImpl::Connect() {
  if (mode_ == MODE_SERVER && uses_fifo_) {
    if (server_listen_pipe_ == -1) {
      return false;
    }
    MessageLoopForIO::current()->WatchFileDescriptor(
        server_listen_pipe_,
        true,
        MessageLoopForIO::WATCH_READ,
        &server_listen_connection_watcher_,
        this);
  } else {
    if (pipe_ == -1) {
      return false;
    }
    MessageLoopForIO::current()->WatchFileDescriptor(
        pipe_,
        true,
        MessageLoopForIO::WATCH_READ,
        &read_watcher_,
        this);
    waiting_connect_ = false;
  }

  if (!waiting_connect_)
    return ProcessOutgoingMessages();
  return true;
}

bool Channel::ChannelImpl::ProcessIncomingMessages() {
  ssize_t bytes_read = 0;

  struct msghdr msg = {0};
  struct iovec iov = {input_buf_, Channel::kReadBufferSize};

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = input_cmsg_buf_;
  msg.msg_controllen = sizeof(input_cmsg_buf_);

  for (;;) {
    if (bytes_read == 0) {
      if (pipe_ == -1)
        return false;

      // Read from pipe.
      // recvmsg() returns 0 if the connection has closed or EAGAIN if no data
      // is waiting on the pipe.
      do {
        bytes_read = recvmsg(pipe_, &msg, MSG_DONTWAIT);
      } while (bytes_read == -1 && errno == EINTR);

      if (bytes_read < 0) {
        if (errno == EAGAIN) {
          return true;
        } else {
          LOG(ERROR) << "pipe error (" << pipe_ << "): " << strerror(errno);
          return false;
        }
      } else if (bytes_read == 0) {
        // The pipe has closed...
        Close();
        return false;
      }
    }
    DCHECK(bytes_read);

    if (client_pipe_ != -1) {
      Singleton<PipeMap>()->Remove(pipe_name_);
      close(client_pipe_);
      client_pipe_ = -1;
    }

    // a pointer to an array of |num_wire_fds| file descriptors from the read
    const int* wire_fds = NULL;
    unsigned num_wire_fds = 0;

    // walk the list of control messages and, if we find an array of file
    // descriptors, save a pointer to the array
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET &&
          cmsg->cmsg_type == SCM_RIGHTS) {
        const unsigned payload_len = cmsg->cmsg_len - CMSG_LEN(0);
        DCHECK(payload_len % sizeof(int) == 0);
        wire_fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
        num_wire_fds = payload_len / 4;

        if (msg.msg_flags & MSG_CTRUNC) {
          LOG(ERROR) << "SCM_RIGHTS message was truncated"
                     << " cmsg_len:" << cmsg->cmsg_len
                     << " fd:" << pipe_;
          for (unsigned i = 0; i < num_wire_fds; ++i)
            close(wire_fds[i]);
          return false;
        }
        break;
      }
    }

    // Process messages from input buffer.
    const char *p;
    const char *end;
    if (input_overflow_buf_.empty()) {
      p = input_buf_;
      end = p + bytes_read;
    } else {
      if (input_overflow_buf_.size() >
         static_cast<size_t>(kMaximumMessageSize - bytes_read)) {
        input_overflow_buf_.clear();
        LOG(ERROR) << "IPC message is too big";
        return false;
      }
      input_overflow_buf_.append(input_buf_, bytes_read);
      p = input_overflow_buf_.data();
      end = p + input_overflow_buf_.size();
    }

    // A pointer to an array of |num_fds| file descriptors which includes any
    // fds that have spilled over from a previous read.
    const int* fds;
    unsigned num_fds;
    unsigned fds_i = 0;  // the index of the first unused descriptor

    if (input_overflow_fds_.empty()) {
      fds = wire_fds;
      num_fds = num_wire_fds;
    } else {
      const size_t prev_size = input_overflow_fds_.size();
      input_overflow_fds_.resize(prev_size + num_wire_fds);
      memcpy(&input_overflow_fds_[prev_size], wire_fds,
             num_wire_fds * sizeof(int));
      fds = &input_overflow_fds_[0];
      num_fds = input_overflow_fds_.size();
    }

    while (p < end) {
      const char* message_tail = Message::FindNext(p, end);
      if (message_tail) {
        int len = static_cast<int>(message_tail - p);
        const Message m(p, len);
        if (m.header()->num_fds) {
          // the message has file descriptors
          if (m.header()->num_fds > num_fds - fds_i) {
            // the message has been completely received, but we didn't get
            // enough file descriptors.
            LOG(WARNING) << "Message needs unreceived descriptors"
                         << " channel:" << this
                         << " message-type:" << m.type()
                         << " header()->num_fds:" << m.header()->num_fds
                         << " num_fds:" << num_fds
                         << " fds_i:" << fds_i;
            // close the existing file descriptors so that we don't leak them
            for (unsigned i = fds_i; i < num_fds; ++i)
              close(fds[i]);
            input_overflow_fds_.clear();
            return false;
          }

          m.descriptor_set()->SetDescriptors(&fds[fds_i], m.header()->num_fds);
          fds_i += m.header()->num_fds;
        }
#ifdef IPC_MESSAGE_DEBUG_EXTRA
        DLOG(INFO) << "received message on channel @" << this <<
                      " with type " << m.type();
#endif
        if (m.routing_id() == MSG_ROUTING_NONE &&
            m.type() == HELLO_MESSAGE_TYPE) {
          // The Hello message contains only the process id.
          listener_->OnChannelConnected(MessageIterator(m).NextInt());
        } else {
          listener_->OnMessageReceived(m);
        }
        p = message_tail;
      } else {
        // Last message is partial.
        break;
      }
    }
    input_overflow_buf_.assign(p, end - p);
    input_overflow_fds_ = std::vector<int>(&fds[fds_i], &fds[num_fds]);

    bytes_read = 0;  // Get more data.
  }

  return true;
}

bool Channel::ChannelImpl::ProcessOutgoingMessages() {
  DCHECK(!waiting_connect_);  // Why are we trying to send messages if there's
                              // no connection?
  is_blocked_on_write_ = false;

  if (output_queue_.empty())
    return true;

  if (pipe_ == -1)
    return false;

  // Write out all the messages we can till the write blocks or there are no
  // more outgoing messages.
  while (!output_queue_.empty()) {
    Message* msg = output_queue_.front();

    size_t amt_to_write = msg->size() - message_send_bytes_written_;
    DCHECK(amt_to_write != 0);
    const char *out_bytes = reinterpret_cast<const char*>(msg->data()) +
        message_send_bytes_written_;
    ssize_t bytes_written = -1;
    do {
      struct msghdr msgh = {0};
      struct iovec iov = {const_cast<char*>(out_bytes), amt_to_write};
      msgh.msg_iov = &iov;
      msgh.msg_iovlen = 1;
      char buf[CMSG_SPACE(
          sizeof(int[DescriptorSet::MAX_DESCRIPTORS_PER_MESSAGE]))];

      if (message_send_bytes_written_ == 0 &&
          !msg->descriptor_set()->empty()) {
        // This is the first chunk of a message which has descriptors to send
        struct cmsghdr *cmsg;
        const unsigned num_fds = msg->descriptor_set()->size();

        DCHECK_LE(num_fds, DescriptorSet::MAX_DESCRIPTORS_PER_MESSAGE);

        msgh.msg_control = buf;
        msgh.msg_controllen = CMSG_SPACE(sizeof(int) * num_fds);
        cmsg = CMSG_FIRSTHDR(&msgh);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * num_fds);
        msg->descriptor_set()->GetDescriptors(
            reinterpret_cast<int*>(CMSG_DATA(cmsg)));
        msgh.msg_controllen = cmsg->cmsg_len;

        msg->header()->num_fds = num_fds;
      }

      bytes_written = sendmsg(pipe_, &msgh, MSG_DONTWAIT);
      if (bytes_written > 0)
        msg->descriptor_set()->CommitAll();
    } while (bytes_written == -1 && errno == EINTR);

    if (bytes_written < 0 && errno != EAGAIN) {
      LOG(ERROR) << "pipe error: " << strerror(errno);
      return false;
    }

    if (static_cast<size_t>(bytes_written) != amt_to_write) {
      if (bytes_written > 0) {
        // If write() fails with EAGAIN then bytes_written will be -1.
        message_send_bytes_written_ += bytes_written;
      }

      // Tell libevent to call us back once things are unblocked.
      is_blocked_on_write_ = true;
      MessageLoopForIO::current()->WatchFileDescriptor(
          pipe_,
          false,  // One shot
          MessageLoopForIO::WATCH_WRITE,
          &write_watcher_,
          this);
      return true;
    } else {
      message_send_bytes_written_ = 0;

      // Message sent OK!
#ifdef IPC_MESSAGE_DEBUG_EXTRA
      DLOG(INFO) << "sent message @" << msg << " on channel @" << this <<
                    " with type " << msg->type();
#endif
      output_queue_.pop();
      delete msg;
    }
  }
  return true;
}

bool Channel::ChannelImpl::Send(Message* message) {
  chrome::Counters::ipc_send_counter().Increment();
#ifdef IPC_MESSAGE_DEBUG_EXTRA
  DLOG(INFO) << "sending message @" << message << " on channel @" << this
             << " with type " << message->type()
             << " (" << output_queue_.size() << " in queue)";
#endif

// TODO(playmobil): implement
//  #ifdef IPC_MESSAGE_LOG_ENABLED
//    Logging::current()->OnSendMessage(message, L"");
//  #endif

  output_queue_.push(message);
  if (!waiting_connect_) {
    if (!is_blocked_on_write_) {
      if (!ProcessOutgoingMessages())
        return false;
    }
  }

  return true;
}

void Channel::ChannelImpl::GetClientFileDescriptorMapping(int *src_fd,
                                                          int *dest_fd) {
  DCHECK(mode_ == MODE_SERVER);
  *src_fd = client_pipe_;
  *dest_fd = kClientChannelFd;
}

void Channel::ChannelImpl::OnClientConnected() {
  // WARNING: this isn't actually called when a client connects.
  DCHECK(mode_ == MODE_SERVER);
}

// Called by libevent when we can read from th pipe without blocking.
void Channel::ChannelImpl::OnFileCanReadWithoutBlocking(int fd) {
  bool send_server_hello_msg = false;
  if (waiting_connect_ && mode_ == MODE_SERVER) {
    // In the case of a socketpair() the server starts listening on its end
    // of the pipe in Connect().
    DCHECK(uses_fifo_);

    if (!ServerAcceptFifoConnection(server_listen_pipe_, &pipe_)) {
      Close();
    }

    // No need to watch the listening socket any longer since only one client
    // can connect.  So unregister with libevent.
    server_listen_connection_watcher_.StopWatchingFileDescriptor();

    // Start watching our end of the socket.
    MessageLoopForIO::current()->WatchFileDescriptor(
        pipe_,
        true,
        MessageLoopForIO::WATCH_READ,
        &read_watcher_,
        this);

    waiting_connect_ = false;
    send_server_hello_msg = true;
  }

  if (!waiting_connect_ && fd == pipe_) {
    if (!ProcessIncomingMessages()) {
      Close();
      listener_->OnChannelError();
    }
  }

  // If we're a server and handshaking, then we want to make sure that we
  // only send our handshake message after we've processed the client's.
  // This gives us a chance to kill the client if the incoming handshake
  // is invalid.
  if (send_server_hello_msg) {
    // This should be our first write so there's no chance we can block here...
    DCHECK(is_blocked_on_write_ == false);
    ProcessOutgoingMessages();
  }
}

// Called by libevent when we can write to the pipe without blocking.
void Channel::ChannelImpl::OnFileCanWriteWithoutBlocking(int fd) {
  if (!ProcessOutgoingMessages()) {
    Close();
    listener_->OnChannelError();
  }
}

void Channel::ChannelImpl::Close() {
  // Close can be called multiple time, so we need to make sure we're
  // idempotent.

  // Unregister libevent for the listening socket and close it.
  server_listen_connection_watcher_.StopWatchingFileDescriptor();

  if (server_listen_pipe_ != -1) {
    close(server_listen_pipe_);
    server_listen_pipe_ = -1;
  }

  // Unregister libevent for the FIFO and close it.
  read_watcher_.StopWatchingFileDescriptor();
  write_watcher_.StopWatchingFileDescriptor();
  if (pipe_ != -1) {
    close(pipe_);
    pipe_ = -1;
  }
  if (client_pipe_ != -1) {
    Singleton<PipeMap>()->Remove(pipe_name_);
    close(client_pipe_);
    client_pipe_ = -1;
  }

  // Unlink the FIFO
  unlink(pipe_name_.c_str());

  while (!output_queue_.empty()) {
    Message* m = output_queue_.front();
    output_queue_.pop();
    delete m;
  }

  // Close any outstanding, received file descriptors
  for (std::vector<int>::iterator
       i = input_overflow_fds_.begin(); i != input_overflow_fds_.end(); ++i) {
    close(*i);
  }
  input_overflow_fds_.clear();
}

//------------------------------------------------------------------------------
// Channel's methods simply call through to ChannelImpl.
Channel::Channel(const std::wstring& channel_id, Mode mode,
                 Listener* listener)
    : channel_impl_(new ChannelImpl(channel_id, mode, listener)) {
}

Channel::~Channel() {
  delete channel_impl_;
}

bool Channel::Connect() {
  return channel_impl_->Connect();
}

void Channel::Close() {
  channel_impl_->Close();
}

void Channel::set_listener(Listener* listener) {
  channel_impl_->set_listener(listener);
}

bool Channel::Send(Message* message) {
  return channel_impl_->Send(message);
}

void Channel::GetClientFileDescriptorMapping(int *src_fd, int *dest_fd) {
  return channel_impl_->GetClientFileDescriptorMapping(src_fd, dest_fd);
}

void Channel::OnClientConnected() {
  return channel_impl_->OnClientConnected();
}


}  // namespace IPC
