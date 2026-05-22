#include "content/net/event_channel.hpp"

#include <cstdio>

namespace studio::content::net
{

void EventChannel::register_event(std::uint8_t tag,
                                  Factory factory,
                                  Handler handler)
{
    registry_[tag] = Entry{ std::move(factory), std::move(handler) };
}

void EventChannel::tick()
{
    while (auto ev = reliable_.poll()) {
        auto it = registry_.find(ev->class_tag);
        if (it == registry_.end()) {
            std::fprintf(stderr,
                "EventChannel: no handler for class tag %u (payload %zu B)\n",
                static_cast<unsigned>(ev->class_tag), ev->payload.size());
            continue;
        }
        auto typed = it->second.factory();
        if (!typed) continue;
        BitStream s(ev->payload.data(), ev->payload.size());
        typed->unpack(s);
        if (s.is_valid()) {
            it->second.handler(*typed);
        }
    }
}

} // namespace studio::content::net
