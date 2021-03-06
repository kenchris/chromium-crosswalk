// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MEDIA_DESKTOP_STREAMS_REGISTRY_H_
#define CHROME_BROWSER_MEDIA_DESKTOP_STREAMS_REGISTRY_H_

#include <map>
#include <string>

#include "chrome/browser/media/desktop_media_picker_model.h"
#include "url/gurl.h"

// DesktopStreamsRegistry is used to store accepted desktop media streams for
// Desktop Capture API. Single instance of this class is created per browser in
// MediaCaptureDevicesDispatcher.
class DesktopStreamsRegistry {
 public:
  DesktopStreamsRegistry();
  ~DesktopStreamsRegistry();

  // Adds new stream to the registry. Called by the implementation of
  // desktopCapture.chooseDesktopMedia() API after user has approved access to
  // |source| for the |origin|. Returns identifier of the new stream.
  std::string RegisterStream(const GURL& origin,
                             const content::DesktopMediaID& source);

  // Validates stream identifier specified in getUserMedia(). Returns null
  // DesktopMediaID if the specified |id| is invalid, i.e. wasn't generated
  // using RegisterStream() or if it was generated for a different origin.
  // Otherwise returns ID of the source and removes it from the registry.
  content::DesktopMediaID RequestMediaForStreamId(const std::string& id,
                                                  const GURL& origin);

 private:
  // Type used to store list of accepted desktop media streams.
  struct ApprovedDesktopMediaStream {
    GURL origin;
    content::DesktopMediaID source;
  };
  typedef std::map<std::string, ApprovedDesktopMediaStream> StreamsMap;

  // Helper function that removes an expired stream from the registry.
  void CleanupStream(const std::string& id);

  StreamsMap approved_streams_;

  DISALLOW_COPY_AND_ASSIGN(DesktopStreamsRegistry);
};

#endif  // CHROME_BROWSER_MEDIA_DESKTOP_STREAMS_REGISTRY_H_
