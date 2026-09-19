#pragma once

#include "embed/embed.h"
#include "api/object_builder.h"

#include <functional>
#include <span>
#include <string>

namespace brokit::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

class HostClass {
public:
    struct Slots {
        ev::Persistent* proto = nullptr;
        ev::Persistent* ctor = nullptr;
    };

    void install(const char* name, uint32_t arity, ev::NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate = nullptr);

    void alias(const char* name) const;
    void inherit(const HostClass& base) const;

    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    void setStatic(const char* name, Value v) const;

    void* unwrap(Value val) const { return ev::handleData(val); }

    Value prototype() const;
    Value constructor() const;

    bool installed() const;

private:
    Slots& slots() const;
    const Slots* slotsIfAny() const;
};

} // namespace brokit::api
