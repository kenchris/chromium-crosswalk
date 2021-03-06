// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_IME_MOCK_IME_PROPERTY_HANDLER_H_
#define CHROMEOS_IME_MOCK_IME_PROPERTY_HANDLER_H_

#include "chromeos/dbus/ibus/ibus_property.h"
#include "chromeos/ime/ibus_bridge.h"

namespace chromeos {

class CHROMEOS_EXPORT MockIMEPropertyHandler :
    public IBusPanelPropertyHandlerInterface {
 public:
  MockIMEPropertyHandler();
  virtual ~MockIMEPropertyHandler();

  virtual void RegisterProperties(const IBusPropertyList& properties) OVERRIDE;
  virtual void UpdateProperty(const IBusProperty& property) OVERRIDE;

  int register_properties_call_count() {
    return register_properties_call_count_;
  }

  const IBusPropertyList& last_registered_properties() {
    return last_registered_properties_;
  }

  // Resets all call count.
  void Reset();

 private:
  int register_properties_call_count_;
  IBusPropertyList last_registered_properties_;
};

}  // namespace chromeos

#endif  // CHROMEOS_IME_MOCK_IME_PROPERTY_HANDLER_H_
