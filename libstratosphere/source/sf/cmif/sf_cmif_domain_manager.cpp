/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>

namespace ams::sf::cmif {

    /* Object ids are domain-scoped, not manager-scoped: a mitm domain mirrors
       the target server's per-session ids, and independent target sessions
       assign overlapping ids (a client converting two sessions of one mitm'd
       service typically gets id 1 from both). Entries are therefore found by
       scanning the owning domain's (small) lists rather than indexing a
       manager-global table, and the same id value may be live in any number
       of domains at once. */

    ServerDomainManager::Domain::~Domain() {
        while (!m_entries.empty()) {
            Entry *entry = std::addressof(m_entries.front());
            {
                std::scoped_lock lk(m_manager->m_entry_owner_lock);
                AMS_ABORT_UNLESS(entry->owner == this);
                entry->owner = nullptr;
                entry->id    = InvalidDomainObjectId;
            }
            entry->object.Reset();
            m_entries.pop_front();
            m_manager->m_entry_manager.FreeEntry(entry);
        }
        while (!m_reserved.empty()) {
            Entry *entry = std::addressof(m_reserved.front());
            m_reserved.pop_front();
            entry->id = InvalidDomainObjectId;
            m_manager->m_entry_manager.FreeEntry(entry);
        }
    }

    void ServerDomainManager::Domain::DisposeImpl() {
        ServerDomainManager *manager = m_manager;
        std::destroy_at(this);
        manager->FreeDomain(this);
    }

    ServerDomainManager::Entry *ServerDomainManager::Domain::FindEntryLocked(DomainObjectId id) {
        if (id == InvalidDomainObjectId) {
            return nullptr;
        }
        for (Entry &entry : m_entries) {
            if (entry.id == id) {
                return std::addressof(entry);
            }
        }
        for (Entry &entry : m_reserved) {
            if (entry.id == id) {
                return std::addressof(entry);
            }
        }
        return nullptr;
    }

    Result ServerDomainManager::Domain::ReserveIds(DomainObjectId *out_ids, size_t count) {
        std::scoped_lock lk(m_manager->m_entry_owner_lock);
        for (size_t i = 0; i < count; i++) {
            Entry *entry = m_manager->m_entry_manager.AllocateEntry();
            R_UNLESS(entry != nullptr, sf::cmif::ResultOutOfDomainEntries());
            AMS_ABORT_UNLESS(entry->owner == nullptr);

            /* Generate an id this domain doesn't use yet. The counter makes
               generated ids unique manager-wide; the scan additionally skips
               values a mirrored (mitm) object may already occupy here. */
            DomainObjectId id;
            do {
                id = DomainObjectId{m_manager->m_next_object_id++};
            } while (id == InvalidDomainObjectId || this->FindEntryLocked(id) != nullptr);

            entry->id = id;
            m_reserved.push_back(*entry);
            out_ids[i] = id;
        }
        R_SUCCEED();
    }

    void ServerDomainManager::Domain::ReserveSpecificIds(const DomainObjectId *ids, size_t count) {
        std::scoped_lock lk(m_manager->m_entry_owner_lock);
        for (size_t i = 0; i < count; i++) {
            const auto id = ids[i];
            if (id == InvalidDomainObjectId) {
                continue;
            }

            /* A duplicate id within one domain is a genuine desynchronization
               from the mitm target. */
            AMS_ABORT_UNLESS(this->FindEntryLocked(id) == nullptr);

            Entry *entry = m_manager->m_entry_manager.AllocateEntry();
            AMS_ABORT_UNLESS(entry != nullptr);
            entry->id = id;
            m_reserved.push_back(*entry);
        }
    }

    bool ServerDomainManager::Domain::TryRegisterMirroredObject(DomainObjectId id, ServiceObjectHolder &&obj) {
        std::scoped_lock lk(m_manager->m_entry_owner_lock);

        /* Refuse (rather than abort) on a duplicate or exhaustion; the caller
           falls back to serving the session as a pure forwarder. */
        if (id == InvalidDomainObjectId || this->FindEntryLocked(id) != nullptr) {
            return false;
        }

        Entry *entry = m_manager->m_entry_manager.AllocateEntry();
        if (entry == nullptr) {
            return false;
        }

        entry->id    = id;
        entry->owner = this;
        m_entries.push_back(*entry);
        entry->object = std::move(obj);
        return true;
    }

    void ServerDomainManager::Domain::UnreserveIds(const DomainObjectId *ids, size_t count) {
        std::scoped_lock lk(m_manager->m_entry_owner_lock);
        for (size_t i = 0; i < count; i++) {
            Entry *entry = this->FindEntryLocked(ids[i]);
            AMS_ABORT_UNLESS(entry != nullptr);
            AMS_ABORT_UNLESS(entry->owner == nullptr);
            m_reserved.erase(m_reserved.iterator_to(*entry));
            entry->id = InvalidDomainObjectId;
            m_manager->m_entry_manager.FreeEntry(entry);
        }
    }

    void ServerDomainManager::Domain::RegisterObject(DomainObjectId id, ServiceObjectHolder &&obj) {
        std::scoped_lock lk(m_manager->m_entry_owner_lock);
        Entry *entry = this->FindEntryLocked(id);
        AMS_ABORT_UNLESS(entry != nullptr);
        AMS_ABORT_UNLESS(entry->owner == nullptr);
        m_reserved.erase(m_reserved.iterator_to(*entry));
        entry->owner = this;
        m_entries.push_back(*entry);
        entry->object = std::move(obj);
    }

    ServiceObjectHolder ServerDomainManager::Domain::UnregisterObject(DomainObjectId id) {
        ServiceObjectHolder obj;
        Entry *entry;
        {
            std::scoped_lock lk(m_manager->m_entry_owner_lock);
            entry = this->FindEntryLocked(id);
            if (entry == nullptr || entry->owner != this) {
                return ServiceObjectHolder();
            }
            entry->owner = nullptr;
            entry->id    = InvalidDomainObjectId;
            obj = std::move(entry->object);
            m_entries.erase(m_entries.iterator_to(*entry));
        }
        m_manager->m_entry_manager.FreeEntry(entry);
        return obj;
    }

    ServiceObjectHolder ServerDomainManager::Domain::GetObject(DomainObjectId id) {
        std::scoped_lock lk(m_manager->m_entry_owner_lock);
        Entry *entry = this->FindEntryLocked(id);
        if (entry == nullptr || entry->owner != this) {
            return ServiceObjectHolder();
        }
        return entry->object.Clone();
    }

    ServerDomainManager::EntryManager::EntryManager(DomainEntryStorage *entry_storage, size_t entry_count) : m_lock() {
        m_entries = reinterpret_cast<Entry *>(entry_storage);
        m_num_entries = entry_count;
        for (size_t i = 0; i < m_num_entries; i++) {
            m_free_list.push_back(*std::construct_at(m_entries + i));
        }
    }

    ServerDomainManager::EntryManager::~EntryManager() {
        for (size_t i = 0; i < m_num_entries; i++) {
            std::destroy_at(m_entries + i);
        }
    }

    ServerDomainManager::Entry *ServerDomainManager::EntryManager::AllocateEntry() {
        std::scoped_lock lk(m_lock);

        if (m_free_list.empty()) {
            return nullptr;
        }

        Entry *e = std::addressof(m_free_list.front());
        m_free_list.pop_front();
        return e;
    }

    void ServerDomainManager::EntryManager::FreeEntry(Entry *entry) {
        std::scoped_lock lk(m_lock);
        AMS_ABORT_UNLESS(entry->owner == nullptr);
        AMS_ABORT_UNLESS(!entry->object);
        m_free_list.push_front(*entry);
    }

}
