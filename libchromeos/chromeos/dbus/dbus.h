// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBCHROMEOS_CHROMEOS_DBUS_DBUS_H_
#define LIBCHROMEOS_CHROMEOS_DBUS_DBUS_H_

#include <dbus/dbus-glib.h>
#include <glib-object.h>

#include <algorithm>
#include <string>

#include "base/logging.h"
#include "chromeos/glib/object.h"

struct DBusMessage;
struct DBusConnection;

namespace chromeos {

// \precondition No functions in the dbus namespace can be called before
// ::g_type_init();

namespace dbus {

// \brief   BusConnection manages the ref-count for a ::DBusGConnection*.
//
// A BusConnection has reference semantics bound to a particular communication
// bus.
//
// \models Copyable, Assignable
// \related GetSystemBusConnection()

class BusConnection {
 public:
  typedef ::DBusGConnection* value_type;

  BusConnection(const BusConnection& x)
      : object_(x.object_) {
    if (object_)
      ::dbus_g_connection_ref(object_);
  }

  ~BusConnection() {
    if (object_)
      ::dbus_g_connection_unref(object_);
  }

  BusConnection& operator=(BusConnection x) {
    swap(*this, x);
    return *this;
  }

  const value_type& g_connection() const {
    DCHECK(object_) << "referencing an empty connection";
    return object_;
  }

  operator bool() const {
    return object_;
  }

  bool HasConnection() const {
    return object_;
  }

 private:
  friend void swap(BusConnection& x, BusConnection& y);

  friend class Proxy;
  friend BusConnection GetSystemBusConnection();
  friend BusConnection GetPrivateBusConnection(const char* address);

  // Constructor takes ownership
  explicit BusConnection(::DBusGConnection* x)
      : object_(x) {
  }

  value_type object_;
};

inline void swap(BusConnection& x, BusConnection& y) {
  std::swap(x.object_, y.object_);
}

// \brief Proxy manages the ref-count for a ::DBusGProxy*.
//
// Proxy has reference semantics and represents a connection to on object on
// the bus. A proxy object is constructed with a connection to a bus, a name
// to an entity on the bus, a path to an object owned by the entity, and an
// interface protocol name used to communicate with the object.

class Proxy {
 public:
  typedef ::DBusGProxy* value_type;

  Proxy()
      : object_(NULL) {
  }

  // Set |connect_to_name_owner| true if you'd like to use
  // dbus_g_proxy_new_for_name_owner() rather than dbus_g_proxy_new_for_name().
  Proxy(const BusConnection& connection,
        const char* name,
        const char* path,
        const char* interface,
        bool connect_to_name_owner)
      : object_(GetGProxy(
          connection, name, path, interface, connect_to_name_owner)) {
  }

  // Equivalent to Proxy(connection, name, path, interface, false).
  Proxy(const BusConnection& connection,
        const char* name,
        const char* path,
        const char* interface)
      : object_(GetGProxy(connection, name, path, interface, false)) {
  }

  // Creates a peer proxy using dbus_g_proxy_new_for_peer.
  Proxy(const BusConnection& connection,
        const char* path,
        const char* interface)
      : object_(GetGPeerProxy(connection, path, interface)) {
  }

  Proxy(const Proxy& x)
      : object_(x.object_) {
    if (object_)
      ::g_object_ref(object_);
  }

  ~Proxy() {
    if (object_)
      ::g_object_unref(object_);
  }

  Proxy& operator=(Proxy x) {
    swap(*this, x);
    return *this;
  }

  const char* path() const {
    DCHECK(object_) << "referencing an empty proxy";
    return ::dbus_g_proxy_get_path(object_);
  }

  // gproxy() returns a reference to the underlying ::DBusGProxy*. As this
  // library evolves, the gproxy() will be moved to be private.

  const value_type& gproxy() const {
    DCHECK(object_) << "referencing an empty proxy";
    return object_;
  }

  operator bool() const {
    return object_;
  }

 private:
  static value_type GetGProxy(const BusConnection& connection,
                              const char* name,
                              const char* path,
                              const char* interface,
                              bool connect_to_name_owner);

  static value_type GetGPeerProxy(const BusConnection& connection,
                                  const char* path,
                                  const char* interface);

  operator int() const;  // for safe bool cast
  friend void swap(Proxy& x, Proxy& y);

  value_type object_;
};

inline void swap(Proxy& x, Proxy& y) {
  std::swap(x.object_, y.object_);
}

// \brief RegisterExclusiveService configures a GObject to run as a service on
//  a supplied ::BusConnection.
//
//  RegisterExclusiveService encapsulates the process of configuring the
//  supplied \param object at \param service_path on the \param connection.
//  Exclusivity is ensured by replacing any existing services at that named
//  location and confirming that the connection is the primary owner.
//
//  Type information for the \param object must be installed with
//  dbus_g_object_type_install_info prior to use.

bool RegisterExclusiveService(const BusConnection& connection,
                              const char* interface_name,
                              const char* service_name,
                              const char* service_path,
                              GObject* object);

template <typename F>  // F is a function signature
class MonitorConnection;

template <typename A1>
class MonitorConnection<void (A1)> {
 public:
  MonitorConnection(const Proxy& proxy, const char* name,
                    void (*monitor)(void*, A1), void* object)
      : proxy_(proxy), name_(name), monitor_(monitor), object_(object) {
  }

  static void Run(::DBusGProxy*, A1 x, MonitorConnection* self) {
    self->monitor_(self->object_, x);
  }
  const Proxy& proxy() const {
    return proxy_;
  }
  const std::string& name() const {
    return name_;
  }

 private:
  Proxy proxy_;
  std::string name_;
  void (*monitor_)(void*, A1);
  void* object_;
};

template <typename A1, typename A2>
class MonitorConnection<void (A1, A2)> {
 public:
  MonitorConnection(const Proxy& proxy, const char* name,
                    void (*monitor)(void*, A1, A2), void* object)
      : proxy_(proxy), name_(name), monitor_(monitor), object_(object) {
  }

  static void Run(::DBusGProxy*, A1 x, A2 y, MonitorConnection* self) {
    self->monitor_(self->object_, x, y);
  }
  const Proxy& proxy() const {
    return proxy_;
  }
  const std::string& name() const {
    return name_;
  }

 private:
  Proxy proxy_;
  std::string name_;
  void (*monitor_)(void*, A1, A2);
  void* object_;
};

template <typename A1, typename A2, typename A3>
class MonitorConnection<void (A1, A2, A3)> {
 public:
  MonitorConnection(const Proxy& proxy, const char* name,
                    void (*monitor)(void*, A1, A2, A3), void* object)
      : proxy_(proxy), name_(name), monitor_(monitor), object_(object) {
  }

  static void Run(::DBusGProxy*, A1 x, A2 y, A3 z, MonitorConnection* self) {
    self->monitor_(self->object_, x, y, z);
  }
  const Proxy& proxy() const {
    return proxy_;
  }
  const std::string& name() const {
    return name_;
  }

 private:
  Proxy proxy_;
  std::string name_;
  void (*monitor_)(void*, A1, A2, A3);
  void* object_;
};

template <typename A1, typename A2, typename A3, typename A4>
class MonitorConnection<void (A1, A2, A3, A4)> {
 public:
  MonitorConnection(const Proxy& proxy, const char* name,
                    void (*monitor)(void*, A1, A2, A3, A4), void* object)
      : proxy_(proxy), name_(name), monitor_(monitor), object_(object) {
  }

  static void Run(::DBusGProxy*, A1 x, A2 y, A3 z, A4 w,
                  MonitorConnection* self) {
    self->monitor_(self->object_, x, y, z, w);
  }
  const Proxy& proxy() const {
    return proxy_;
  }
  const std::string& name() const {
    return name_;
  }

 private:
  Proxy proxy_;
  std::string name_;
  void (*monitor_)(void*, A1, A2, A3, A4);
  void* object_;
};

template <typename A1>
MonitorConnection<void (A1)>* Monitor(const Proxy& proxy, const char* name,
                                      void (*monitor)(void*, A1),
                                      void* object) {
  typedef MonitorConnection<void (A1)> ConnectionType;

  ConnectionType* result = new ConnectionType(proxy, name, monitor, object);

  ::dbus_g_proxy_add_signal(proxy.gproxy(), name,
                            glib::type_to_gtypeid<A1>(), G_TYPE_INVALID);
  ::dbus_g_proxy_connect_signal(proxy.gproxy(), name,
                                G_CALLBACK(&ConnectionType::Run),
                                result, NULL);
  return result;
}

template <typename A1, typename A2>
MonitorConnection<void (A1, A2)>* Monitor(const Proxy& proxy, const char* name,
                                      void (*monitor)(void*, A1, A2),
                                      void* object) {
  typedef MonitorConnection<void (A1, A2)> ConnectionType;

  ConnectionType* result = new ConnectionType(proxy, name, monitor, object);

  ::dbus_g_proxy_add_signal(proxy.gproxy(), name,
                            glib::type_to_gtypeid<A1>(),
                            glib::type_to_gtypeid<A2>(),
                            G_TYPE_INVALID);
  ::dbus_g_proxy_connect_signal(proxy.gproxy(), name,
                                G_CALLBACK(&ConnectionType::Run),
                                result, NULL);
  return result;
}

template <typename A1, typename A2, typename A3>
MonitorConnection<void (A1, A2, A3)>* Monitor(const Proxy& proxy,
                                          const char* name,
                                          void (*monitor)(void*, A1, A2, A3),
                                          void* object) {
  typedef MonitorConnection<void (A1, A2, A3)> ConnectionType;

  ConnectionType* result = new ConnectionType(proxy, name, monitor, object);

  ::dbus_g_proxy_add_signal(proxy.gproxy(), name,
                            glib::type_to_gtypeid<A1>(),
                            glib::type_to_gtypeid<A2>(),
                            glib::type_to_gtypeid<A3>(),
                            G_TYPE_INVALID);
  ::dbus_g_proxy_connect_signal(proxy.gproxy(), name,
                                G_CALLBACK(&ConnectionType::Run),
                                result, NULL);
  return result;
}

template <typename A1, typename A2, typename A3, typename A4>
MonitorConnection<void (A1, A2, A3, A4)>* Monitor(const Proxy& proxy,
    const char* name,
    void (*monitor)(void*, A1, A2, A3, A4),
    void* object) {
  typedef MonitorConnection<void (A1, A2, A3, A4)> ConnectionType;

  ConnectionType* result = new ConnectionType(proxy, name, monitor, object);

  ::dbus_g_proxy_add_signal(proxy.gproxy(), name,
                            glib::type_to_gtypeid<A1>(),
                            glib::type_to_gtypeid<A2>(),
                            glib::type_to_gtypeid<A3>(),
                            glib::type_to_gtypeid<A4>(),
                            G_TYPE_INVALID);
  ::dbus_g_proxy_connect_signal(proxy.gproxy(), name,
                                G_CALLBACK(&ConnectionType::Run),
                                result, NULL);
  return result;
}

template <typename F>
void Disconnect(MonitorConnection<F>* connection) {
  typedef MonitorConnection<F> ConnectionType;

  ::dbus_g_proxy_disconnect_signal(connection->proxy().gproxy(),
                                   connection->name().c_str(),
                                   G_CALLBACK(&ConnectionType::Run),
                                   connection);
  delete connection;
}

// \brief call_PtrArray() invokes a method on a proxy returning a
//  glib::PtrArray.
//
// CallPtrArray is the first instance of what is likely to be a general
// way to make method calls to a proxy. It will likely be replaced with
// something like Call(proxy, method, arg1, arg2, ..., ResultType*) in the
// future. However, I don't yet have enough cases to generalize from.

bool CallPtrArray(const Proxy& proxy,
                  const char* method,
                  glib::ScopedPtrArray<const char*>* result);

// \brief RetrieveProperty() retrieves a property of an object associated with a
//  proxy.
//
// Given a proxy to an object supporting the org.freedesktop.DBus.Properties
// interface, the RetrieveProperty() call will retrieve a property of the
// specified interface on the object storing it in \param result and returning
// \true. If the dbus call fails or the object returned is not of type \param T,
// then \false is returned and \param result is unchanged.
//
// \example
// Proxy proxy(GetSystemBusConnection(),
//             "org.freedesktop.DeviceKit.Power", // A named entity on the bus
//             battery_name,  // Path to a battery on the bus
//             "org.freedesktop.DBus.Properties") // Properties interface
//
// double x;
// if (RetrieveProperty(proxy,
//                      "org.freedesktop.DeviceKit.Power.Device",
//                      "percentage")
//   std::cout << "Battery charge is " << x << "% of capacity.";
// \end_example

template <typename T>
bool RetrieveProperty(const Proxy& proxy,
                      const char* interface,
                      const char* property,
                      T* result) {
  glib::ScopedError error;
  glib::Value value;

  if (!::dbus_g_proxy_call(proxy.gproxy(), "Get", &Resetter(&error).lvalue(),
                           G_TYPE_STRING, interface,
                           G_TYPE_STRING, property,
                           G_TYPE_INVALID,
                           G_TYPE_VALUE, &value,
                           G_TYPE_INVALID)) {
    LOG(ERROR) << "Getting property failed: "
               << (error->message ? error->message : "Unknown Error.");
    return false;
  }
  return glib::Retrieve(value, result);
}

// \brief RetrieveProperties returns a HashTable of all properties for the
// specified interface.

bool RetrieveProperties(const Proxy& proxy,
                        const char* interface,
                        glib::ScopedHashTable* result);

// \brief Returns a connection to the system bus.

BusConnection GetSystemBusConnection();

// \brief Returns a private connection to a bus at |address|.

BusConnection GetPrivateBusConnection(const char* address);

// \brief Calls a method |method_name| with no arguments per the given |path|
// and |interface_name|.  Ignores return value.

void CallMethodWithNoArguments(const char* service_name,
                               const char* path,
                               const char* interface_name,
                               const char* method_name);

// \brief Low-level signal monitor base class.
//
// Used when there is no definite named signal sender (that Proxy
// could be used for).

class SignalWatcher {
 public:
  SignalWatcher() {}
  ~SignalWatcher();
  void StartMonitoring(const std::string& interface, const std::string& signal);

 private:
  // Callback invoked on the given signal arrival.
  virtual void OnSignal(DBusMessage* message) = 0;

  // Returns a string matching the D-Bus messages that we want to listen for.
  std::string GetDBusMatchString() const;

  // A D-Bus message filter to receive signals.
  static DBusHandlerResult FilterDBusMessage(DBusConnection* dbus_conn,
                                             DBusMessage* message,
                                             void* data);
  std::string interface_;
  std::string signal_;
};

}  // namespace dbus
}  // namespace chromeos

#endif  // LIBCHROMEOS_CHROMEOS_DBUS_DBUS_H_
