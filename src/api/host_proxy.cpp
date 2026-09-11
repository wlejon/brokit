#include "api/host_proxy.h"
#include "api/object_builder.h"
#include "api/arg_reader.h"

namespace brokit::api {

namespace {

struct TrapPack {
    HostProxyTraps t;
    ev::Persistent methods;
};

}  // namespace

Value makeHostProxy(HostProxyTraps traps) {
    auto pack = std::make_shared<TrapPack>();
    pack->t = std::move(traps);
    pack->methods.set(pack->t.methods);

    ObjectBuilder h;

    // get(target, key, receiver)
    h.def("get", 3, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::undefined();
        const std::string k = ev::toUtf8(key);
        if (!ev::isUndefined(pack->methods.get())) {
            Value m = ev::getProperty(pack->methods.get(), k.c_str());
            if (!ev::isUndefined(m)) return m;
        }
        Value out = ev::undefined();
        if (pack->t.get && pack->t.get(k, out)) return out;
        return ev::undefined();
    });

    // set(target, key, value, receiver)
    h.def("set", 4, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::fromBool(true);
        const std::string k = ev::toUtf8(key);
        if (pack->t.set) pack->t.set(k, argAt(a, 2));
        return ev::fromBool(true);
    });

    h.def("has", 2, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::fromBool(false);
        const std::string k = ev::toUtf8(key);
        if (!ev::isUndefined(pack->methods.get()) &&
            !ev::isUndefined(ev::getProperty(pack->methods.get(), k.c_str()))) {
            return ev::fromBool(true);
        }
        return ev::fromBool(pack->t.has && pack->t.has(k));
    });

    h.def("deleteProperty", 2, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::fromBool(true);
        const std::string k = ev::toUtf8(key);
        if (pack->t.remove) pack->t.remove(k);
        return ev::fromBool(true);
    });

    h.def("ownKeys", 1, [pack](Value, std::span<const Value>) -> Value {
        std::vector<std::string> keys;
        if (pack->t.ownKeys) keys = pack->t.ownKeys();
        return hostArrayOf(keys.size(),
                           [&keys](size_t i) { return ev::fromUtf8(keys[i]); });
    });

    h.def("getOwnPropertyDescriptor", 2, [pack](Value, std::span<const Value> a) -> Value {
        Value key = argAt(a, 1);
        if (ev::isSymbol(key)) return ev::undefined();
        const std::string k = ev::toUtf8(key);
        if (!(pack->t.has && pack->t.has(k))) return ev::undefined();
        Value out = ev::undefined();
        if (pack->t.get) pack->t.get(k, out);
        ObjectBuilder d;
        d.set("value", out);
        d.set("writable", ev::fromBool(true));
        d.set("enumerable", ev::fromBool(true));
        d.set("configurable", ev::fromBool(true));
        return d.get();
    });

    if (pack->t.apply) {
        h.def("apply", 3, [pack](Value, std::span<const Value> a) -> Value {
            Value list = argAt(a, 2);
            const uint32_t n = ev::isObject(list)
                                   ? static_cast<uint32_t>(
                                         ev::toDouble(ev::getProperty(list, "length")))
                                   : 0;
            std::vector<Value> args;
            args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(ev::getElement(list, i));
            return pack->t.apply(argAt(a, 1), std::span<const Value>(args));
        });
    }
    if (pack->t.construct) {
        h.def("construct", 3, [pack](Value, std::span<const Value> a) -> Value {
            Value list = argAt(a, 1);
            const uint32_t n = ev::isObject(list)
                                   ? static_cast<uint32_t>(
                                         ev::toDouble(ev::getProperty(list, "length")))
                                   : 0;
            std::vector<Value> args;
            args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(ev::getElement(list, i));
            return pack->t.construct(std::span<const Value>(args));
        });
    }

    ev::Persistent handler(h.get());
    ev::Persistent target(ev::isUndefined(pack->t.target) ? ev::createObject()
                                                          : pack->t.target);

    ev::GlobalValue proxyCtor = ev::globalValue("Proxy");
    if (!proxyCtor.found) {
        return target.get();
    }
    ev::Persistent ctor(proxyCtor.value);

    const Value args[2] = {target.get(), handler.get()};
    ev::CallResult r = ev::construct(ctor.get(), std::span<const Value>(args, 2));
    if (r.thrown) return target.get();
    return r.value;
}

} // namespace brokit::api
