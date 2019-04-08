/*
 * @CopyRight:
 * FISCO-BCOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FISCO-BCOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>
 * (c) 2016-2018 fisco-dev contributors.
 */
/** @file MemoryTable2.cpp
 *  @author ancelmo
 *  @date 20180921
 */
#include "MemoryTable2.h"
#include "Common.h"
#include "Table.h"
#include <arpa/inet.h>
#include <json/json.h>
#include <libdevcore/FixedHash.h>
#include <libdevcore/easylog.h>
#include <libdevcrypto/Hash.h>
#include <libprecompiled/Common.h>
#include <boost/exception/diagnostic_information.hpp>
#include <boost/lexical_cast.hpp>
#include <algorithm>
#include <thread>
#include <vector>

using namespace dev;
using namespace dev::storage;
using namespace dev::precompiled;

MemoryTable2::MemoryTable2() : m_newEntries(std::make_shared<EntriesType>()) {}

Entries::Ptr MemoryTable2::select(const std::string& key, Condition::Ptr condition)
{
    try
    {
        auto entries = std::make_shared<Entries>();
        condition->EQ(m_tableInfo->key, key);
        if (m_remoteDB)
        {
            // query remoteDB anyway
            Entries::Ptr dbEntries =
                m_remoteDB->select(m_blockHash, m_blockNum, m_tableInfo->name, key, condition);

            if (!dbEntries)
            {
                return entries;
            }

            for (size_t i = 0; i < dbEntries->size(); ++i)
            {
                auto entryIt = m_dirty.find(dbEntries->get(i)->getID());
                if (entryIt != m_dirty.end())
                {
                    entries->addEntry(entryIt->second);
                }
                else
                {
                    entries->addEntry(dbEntries->get(i));
                }
            }
        }

        auto indices = processEntries(m_newEntries, condition);
        for (auto it : indices)
        {
            m_newEntries->get(it)->setTempIndex(it);
            entries->addEntry(m_newEntries->get(it));
        }

        return entries;
    }
    catch (std::exception& e)
    {
        STORAGE_LOG(ERROR) << LOG_BADGE("MemoryTable2") << LOG_DESC("Table select failed for")
                           << LOG_KV("msg", boost::diagnostic_information(e));
    }

    return std::make_shared<Entries>();
}

int MemoryTable2::update(
    const std::string& key, Entry::Ptr entry, Condition::Ptr condition, AccessOptions::Ptr options)
{
    try
    {
        if (options->check && !checkAuthority(options->origin))
        {
            STORAGE_LOG(WARNING) << LOG_BADGE("MemoryTable2")
                                 << LOG_DESC("update permission denied")
                                 << LOG_KV("origin", options->origin.hex()) << LOG_KV("key", key);

            return storage::CODE_NO_AUTHORIZED;
        }

        checkField(entry);

        auto entries = select(key, condition);
        std::vector<Change::Record> records;

        for (size_t i = 0; i < entries->size(); ++i)
        {
            auto updateEntry = entries->get(i);

            // if id not equals to zero and not in the m_dirty, must be new dirty entry
            if (updateEntry->getID() != 0 && m_dirty.find(updateEntry->getID()) == m_dirty.end())
            {
                m_dirty.insert(std::make_pair(updateEntry->getID(), updateEntry));
            }

            for (auto& it : *(entry->fields()))
            {
                //_id_ always got initialized value 0 from Entry::Entry()
                // no need to update _id_ while updating entry
                if (it.first != "_id_")
                {
                    records.emplace_back(updateEntry->getTempIndex(), it.first,
                        updateEntry->getField(it.first), updateEntry->getID());
                    updateEntry->setField(it.first, it.second);
                }
            }
        }

        m_recorder(shared_from_this(), Change::Update, key, records);

        return entries->size();
    }
    catch (std::exception& e)
    {
        STORAGE_LOG(ERROR) << LOG_BADGE("MemoryTable2")
                           << LOG_DESC("Access MemoryTable2 failed for")
                           << LOG_KV("msg", boost::diagnostic_information(e));
    }

    return 0;
}

int MemoryTable2::insert(
    const std::string& key, Entry::Ptr entry, AccessOptions::Ptr options, bool needSelect)
{
    try
    {
        (void)needSelect;

        if (options->check && !checkAuthority(options->origin))
        {
            STORAGE_LOG(WARNING) << LOG_BADGE("MemoryTable2")
                                 << LOG_DESC("insert permission denied")
                                 << LOG_KV("origin", options->origin.hex()) << LOG_KV("key", key);
            return storage::CODE_NO_AUTHORIZED;
        }

        checkField(entry);

        entry->setField(m_tableInfo->key, key);
        // Change::Record record(m_newEntries->size());
        auto iter = m_newEntries->addEntry(entry);
        Change::Record record(iter - m_newEntries->begin());

        std::vector<Change::Record> value{record};
        m_recorder(shared_from_this(), Change::Insert, key, value);

        return 1;
    }
    catch (std::exception& e)
    {
        STORAGE_LOG(ERROR) << LOG_BADGE("MemoryTable2")
                           << LOG_DESC("Access MemoryTable2 failed for")
                           << LOG_KV("msg", boost::diagnostic_information(e));
    }

    return 0;
}

int MemoryTable2::remove(
    const std::string& key, Condition::Ptr condition, AccessOptions::Ptr options)
{
    try
    {
        if (options->check && !checkAuthority(options->origin))
        {
            STORAGE_LOG(WARNING) << LOG_BADGE("MemoryTable2")
                                 << LOG_DESC("remove permission denied")
                                 << LOG_KV("origin", options->origin.hex()) << LOG_KV("key", key);
            return storage::CODE_NO_AUTHORIZED;
        }

        auto entries = select(key, condition);

        std::vector<Change::Record> records;
        for (size_t i = 0; i < entries->size(); ++i)
        {
            auto removeEntry = entries->get(i);

            removeEntry->setStatus(1);

            // if id not equals to zero and not in the m_dirty, must be new dirty entry
            if (removeEntry->getID() != 0 && m_dirty.find(removeEntry->getID()) == m_dirty.end())
            {
                m_dirty.insert(std::make_pair(removeEntry->getID(), removeEntry));
            }

            records.emplace_back(removeEntry->getTempIndex(), "", "", removeEntry->getID());
        }

        m_recorder(shared_from_this(), Change::Remove, key, records);

        return entries->size();
    }
    catch (std::exception& e)
    {
        STORAGE_LOG(ERROR) << LOG_BADGE("MemoryTable2")
                           << LOG_DESC("Access MemoryTable2 failed for")
                           << LOG_KV("msg", boost::diagnostic_information(e));
    }

    return 0;
}

dev::h256 MemoryTable2::hash()
{
    bytes data;
    auto tempEntries = std::vector<Entry::Ptr>();
    auto size = (m_dirty.size() + m_newEntries->size());
    tempEntries.reserve(size);

    auto comparator = [this](const Entry::Ptr& lhs, const Entry::Ptr& rhs) {
        auto ret = lhs->getField(m_tableInfo->key).compare(rhs->getField(m_tableInfo->key));
        if (ret > 0)
        {
            return true;
        }
        else
        {
            if (ret == 0)
            {
                auto& lFields = *lhs->fields();
                auto& rFields = *rhs->fields();
                if (lFields.size() > rFields.size())
                {
                    return true;
                }
                else
                {
                    if (lFields.size() == rFields.size())
                    {
                        for (auto lIter = lFields.begin(), rIter = rFields.begin();
                             lIter != lFields.end() && rIter != rFields.end(); ++lIter, ++rIter)
                        {
                            if (lIter->first != rIter->first)
                            {
                                return static_cast<bool>(lIter->first.compare(rIter->first));
                            }

                            if (lIter->second != rIter->second)
                            {
                                return static_cast<bool>(lIter->second.compare(rIter->second));
                            }
                        }
                        return false;
                    }
                    else
                    {
                        return false;
                    }
                }
            }
            else
            {
                return false;
            }
        }
    };

    for (auto it : m_dirty)
    {
        auto entry = it.second;
        tempEntries.push_back(entry);
    }

    std::sort(tempEntries.begin(), tempEntries.begin() + m_dirty.size(), comparator);

    for (size_t i = 0; i < m_newEntries->size(); ++i)
    {
        auto entry = m_newEntries->get(i);
        while (entry.get() == nullptr)
        {
            std::this_thread::yield();
        }
        tempEntries.push_back(entry);
    }

    std::sort(tempEntries.begin() + m_dirty.size(), tempEntries.begin() + size, comparator);

    for (size_t i = 0; i < size; ++i)
    {
        auto entry = tempEntries[i];
        for (auto fieldIt : *(entry->fields()))
        {
            if (isHashField(fieldIt.first))
            {
                data.insert(data.end(), fieldIt.first.begin(), fieldIt.first.end());
                data.insert(data.end(), fieldIt.second.begin(), fieldIt.second.end());
            }
        }
    }

    if (data.empty())
    {
        return h256();
    }

    bytesConstRef bR(data.data(), data.size());
    h256 hash = dev::sha256(bR);

    return hash;
}

void MemoryTable2::rollback(const Change& _change)
{
    LOG(TRACE) << "Before rollback newEntries size: " << m_newEntries->size();
    for (size_t i = 0; i < m_newEntries->size(); ++i)
    {
        auto entry = m_newEntries->get(i);

        std::stringstream ss;
        ss << i;
        ss << "," << entry->getStatus();

        for (auto it : *(entry->fields()))
        {
            ss << "," << it.first << ":" << it.second;
        }
        LOG(TRACE) << ss.str();
    }

    switch (_change.kind)
    {
    case Change::Insert:
    {
        LOG(TRACE) << "Rollback insert record newIndex: " << _change.value[0].index;

        auto entry = m_newEntries->get(_change.value[0].index);
        entry->setStatus(1);
        // m_newEntries->removeEntry(_change.value[0].newIndex);
        break;
    }
    case Change::Update:
    {
        for (auto& record : _change.value)
        {
            LOG(TRACE) << "Rollback update record id: " << record.id
                       << " newIndex: " << record.index;
            if (record.id)
            {
                auto it = m_dirty.find(record.id);
                if (it != m_dirty.end())
                {
                    it->second->setField(record.key, record.oldValue);
                }
            }
            else
            {
                auto entry = m_newEntries->get(record.index);
                entry->setField(record.key, record.oldValue);
            }
        }
        break;
    }
    case Change::Remove:
    {
        for (auto& record : _change.value)
        {
            LOG(TRACE) << "Rollback remove record id: " << record.id
                       << " newIndex: " << record.index;
            if (record.id)
            {
                auto it = m_dirty.find(record.id);
                if (it != m_dirty.end())
                {
                    it->second->setStatus(0);
                }
            }
            else
            {
                auto entry = m_newEntries->get(record.index);
                entry->setStatus(0);
            }
        }
        break;
    }
    case Change::Select:

    default:
        break;
    }

    LOG(TRACE) << "After rollback newEntries size: " << m_newEntries->size();
    for (size_t i = 0; i < m_newEntries->size(); ++i)
    {
        auto entry = m_newEntries->get(i);

        std::stringstream ss;
        ss << i;
        ss << "," << entry->getStatus();

        for (auto it : *(entry->fields()))
        {
            ss << "," << it.first << ":" << it.second;
        }
        LOG(TRACE) << ss.str();
    }
}
