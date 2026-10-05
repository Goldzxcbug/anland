#pragma once

#include <cstdint>
#include <map>

// Gamescope-native translation only; no wire protocol or Android gesture policy.
// Latch the mode on DOWN until UP. wlroots counts button presses, whereas
// wlserver_touchdown/up's boolean button_held cannot balance multiple contacts.
class AnlandTouchState {
public:
    static constexpr uint32_t Passthrough = UINT32_MAX;
    struct Change {
        bool accepted = false;
        uint32_t button = 0;
        bool edge = false;
    };

    Change down(int id, uint32_t button, bool trackpad = false) {
        if (m_contacts.count(id))
            return {};
        bool held = false;
        for (const auto &contact : m_contacts)
            held |= contact.second.button == button;
        m_contacts.emplace(id, Contact{button, trackpad});
        return {true, button, button != 0 && button != Passthrough && !held};
    }

    Change up(int id) {
        const auto it = m_contacts.find(id);
        if (it == m_contacts.end())
            return {};
        const uint32_t button = it->second.button;
        m_contacts.erase(it);
        bool held = false;
        for (const auto &contact : m_contacts)
            held |= contact.second.button == button;
        return {true, button, button != 0 && button != Passthrough && !held};
    }

    bool contains(int id) const { return m_contacts.count(id) != 0; }
    bool passthrough(int id) const {
        const auto it = m_contacts.find(id);
        return it != m_contacts.end() && it->second.button == Passthrough;
    }
    bool trackpad(int id) const {
        const auto it = m_contacts.find(id);
        return it != m_contacts.end() && it->second.trackpad;
    }
    int first() const { return m_contacts.begin()->first; }
    bool empty() const { return m_contacts.empty(); }

private:
    struct Contact { uint32_t button; bool trackpad; };
    std::map<int, Contact> m_contacts;
};