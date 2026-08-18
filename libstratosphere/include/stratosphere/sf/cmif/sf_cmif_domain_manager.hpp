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

#pragma once
#include <stratosphere/sf/sf_common.hpp>
#include <stratosphere/sf/cmif/sf_cmif_domain_api.hpp>
#include <stratosphere/sf/cmif/sf_cmif_domain_service_object.hpp>
#include <stratosphere/sf/impl/sf_service_object_impl.hpp>

namespace ams::sf::cmif {

    class ServerDomainManager {
        NON_COPYABLE(ServerDomainManager);
        NON_MOVEABLE(ServerDomainManager);
        private:
            class Domain;

            struct Entry {
                NON_COPYABLE(Entry);
                NON_MOVEABLE(Entry);

                util::IntrusiveListNode free_list_node;
                util::IntrusiveListNode domain_list_node;
                Domain *owner;
                /* The id this entry answers to, scoped to its domain. Ids are
                   only meaningful within one domain: a mitm domain mirrors the
                   target server's per-session ids, so the same value can
                   legitimately be live in several domains at once. Entries are
                   therefore looked up by scanning the owning domain's lists,
                   never by a manager-global index. */
                DomainObjectId id;
                ServiceObjectHolder object;

                explicit Entry() : owner(nullptr), id(InvalidDomainObjectId) { /* ... */ }
            };

            class Domain final : public DomainServiceObject, private sf::impl::ServiceObjectImplBase2 {
                NON_COPYABLE(Domain);
                NON_MOVEABLE(Domain);
                private:
                    using EntryList = typename util::IntrusiveListMemberTraits<&Entry::domain_list_node>::ListType;
                private:
                    ServerDomainManager *m_manager;
                    EntryList m_entries;
                    /* Entries with an id assigned (ReserveIds/ReserveSpecificIds)
                       that have not been registered yet. */
                    EntryList m_reserved;
                private:
                    /* Requires m_manager->m_entry_owner_lock held. */
                    Entry *FindEntryLocked(DomainObjectId id);
                public:
                    explicit Domain(ServerDomainManager *m) : m_manager(m) { /* ... */ }
                    ~Domain();

                    void DisposeImpl();

                    virtual void AddReference() override {
                        ServiceObjectImplBase2::AddReferenceImpl();
                    }

                    virtual void Release() override {
                        if (ServiceObjectImplBase2::ReleaseImpl()) {
                            this->DisposeImpl();
                        }
                    }

                    virtual ServerDomainBase *GetServerDomain() override final {
                        return static_cast<ServerDomainBase *>(this);
                    }

                    virtual Result ReserveIds(DomainObjectId *out_ids, size_t count) override final;
                    virtual void   ReserveSpecificIds(const DomainObjectId *ids, size_t count) override final;
                    virtual bool   TryRegisterMirroredObject(DomainObjectId id, ServiceObjectHolder &&obj) override final;
                    virtual void   UnreserveIds(const DomainObjectId *ids, size_t count) override final;
                    virtual void   RegisterObject(DomainObjectId id, ServiceObjectHolder &&obj) override final;

                    virtual ServiceObjectHolder UnregisterObject(DomainObjectId id) override final;
                    virtual ServiceObjectHolder GetObject(DomainObjectId id) override final;
            };
        public:
            using DomainEntryStorage  = util::TypedStorage<Entry>;
            using DomainStorage       = util::TypedStorage<Domain>;
        private:
            class EntryManager {
                private:
                    using EntryList = typename util::IntrusiveListMemberTraits<&Entry::free_list_node>::ListType;
                private:
                    os::SdkMutex m_lock;
                    EntryList m_free_list;
                    Entry *m_entries;
                    size_t m_num_entries;
                public:
                    EntryManager(DomainEntryStorage *entry_storage, size_t entry_count);
                    ~EntryManager();
                    Entry *AllocateEntry();
                    void   FreeEntry(Entry *);

            };
        private:
            os::SdkMutex m_entry_owner_lock;
            EntryManager m_entry_manager;
            /* Source of generated (non-mirrored) object ids. Guarded by
               m_entry_owner_lock. Monotonic, so a generated id can never
               accidentally equal another generated id in any domain. */
            u32 m_next_object_id;
        private:
            virtual void *AllocateDomain()   = 0;
            virtual void  FreeDomain(void *) = 0;
        protected:
            ServerDomainManager(DomainEntryStorage *entry_storage, size_t entry_count) : m_entry_owner_lock(), m_entry_manager(entry_storage, entry_count), m_next_object_id(1) { /* ... */ }

            inline DomainServiceObject *AllocateDomainServiceObject() {
                void *storage = this->AllocateDomain();
                if (storage == nullptr) {
                    return nullptr;
                }

                return std::construct_at(static_cast<Domain *>(storage), this);
            }
        public:
            static void DestroyDomainServiceObject(DomainServiceObject *obj) {
                static_cast<Domain *>(obj)->DisposeImpl();
            }
    };


}
