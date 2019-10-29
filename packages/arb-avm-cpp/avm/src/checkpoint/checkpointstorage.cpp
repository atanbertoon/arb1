/*
 * Copyright 2019, Offchain Labs, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <rocksdb/options.h>
#include <rocksdb/utilities/transaction.h>
#include <array>

#include <avm/checkpoint/checkpointstorage.hpp>

std::tuple<uint32_t, std::vector<unsigned char>> parseCountAndValue(
    const std::string& string_value) {
    if (string_value.empty()) {
        return std::make_tuple(0, std::vector<unsigned char>());
    } else {
        const char* c_string = string_value.c_str();
        uint32_t ref_count;
        memcpy(&ref_count, c_string, sizeof(ref_count));
        std::vector<unsigned char> saved_value(
            string_value.begin() + sizeof(ref_count), string_value.end());

        return std::make_tuple(ref_count, saved_value);
    }
}

std::vector<unsigned char> serializeCountAndValue(
    uint32_t count,
    const std::vector<unsigned char>& value) {
    std::vector<unsigned char> output_vector(sizeof(count));
    memcpy(&output_vector[0], &count, sizeof(count));
    output_vector.insert(output_vector.end(), value.begin(), value.end());

    return output_vector;
}

CheckpointStorage::CheckpointStorage(std::string db_path) {
    rocksdb::TransactionDBOptions txn_options;
    rocksdb::Options options;
    options.create_if_missing = true;

    txn_db_path = std::move(db_path);
    rocksdb::TransactionDB* db = nullptr;
    rocksdb::TransactionDB::Open(options, txn_options, txn_db_path, &db);
    txn_db = std::unique_ptr<rocksdb::TransactionDB>(db);
};

CheckpointStorage::~CheckpointStorage() {
    txn_db->Close();
    DestroyDB(txn_db_path, rocksdb::Options());
}

SaveResults CheckpointStorage::incrementReference(
    const std::vector<unsigned char>& hash_key) {
    auto results = getValue(hash_key);

    if (results.status.ok()) {
        auto updated_count = results.reference_count + 1;
        return saveValueWithRefCount(updated_count, hash_key,
                                     results.stored_value);
    } else {
        return SaveResults{0, results.status, hash_key};
    }
}

SaveResults CheckpointStorage::saveValue(
    const std::vector<unsigned char>& hash_key,
    const std::vector<unsigned char>& value) {
    auto results = getValue(hash_key);
    int ref_count;

    if (results.status.ok()) {
        assert(results.stored_value == value);
        ref_count = results.reference_count + 1;
    } else {
        ref_count = 1;
    }
    return saveValueWithRefCount(ref_count, hash_key, value);
};

DeleteResults CheckpointStorage::deleteValue(
    const std::vector<unsigned char>& hash_key) {
    auto results = getValue(hash_key);

    if (results.status.ok()) {
        if (results.reference_count < 2) {
            auto delete_status = deleteKeyValuePair(hash_key);
            return DeleteResults{0, delete_status};

        } else {
            auto updated_ref_count = results.reference_count - 1;
            auto update_result = saveValueWithRefCount(
                updated_ref_count, hash_key, results.stored_value);
            return DeleteResults{updated_ref_count, update_result.status};
        }
    } else {
        return DeleteResults{0, results.status};
    }
}

GetResults CheckpointStorage::getValue(
    const std::vector<unsigned char>& hash_key) const {
    auto read_options = rocksdb::ReadOptions();
    std::string return_value;
    std::string key_str(hash_key.begin(), hash_key.end());
    auto get_status = txn_db->Get(read_options, key_str, &return_value);

    if (get_status.ok()) {
        auto tuple = parseCountAndValue(return_value);
        auto stored_val = std::get<1>(tuple);
        auto ref_count = std::get<0>(tuple);

        return GetResults{ref_count, get_status, stored_val};
    } else {
        auto unsuccessful = rocksdb::Status().NotFound();
        return GetResults{0, unsuccessful, std::vector<unsigned char>()};
    }
}

Transaction::Transaction(std::unique_ptr<rocksdb::Transaction> transaction_) {
    transaction = std::move(transaction_);
}

Transaction::~Transaction() {
    transaction.reset();
}

rocksdb::Status Transaction::Commit() {
    return transaction->Commit();
}

// private
// ----------------------------------------------------------------------

std::unique_ptr<rocksdb::Transaction> CheckpointStorage::makeTransaction() {
    rocksdb::WriteOptions writeOptions;
    rocksdb::Transaction* transaction = txn_db->BeginTransaction(writeOptions);
    return std::unique_ptr<rocksdb::Transaction>(transaction);
}

SaveResults CheckpointStorage::saveValueWithRefCount(
    uint32_t updated_ref_count,
    const std::vector<unsigned char>& hash_key,
    const std::vector<unsigned char>& value) {
    auto updated_entry = serializeCountAndValue(updated_ref_count, value);

    auto status = saveKeyValuePair(hash_key, updated_entry);

    if (status.ok()) {
        return SaveResults{updated_ref_count, status, hash_key};
    } else {
        return SaveResults{0, status, hash_key};
    }
}

rocksdb::Status CheckpointStorage::saveKeyValuePair(
    const std::vector<unsigned char>& key,
    const std::vector<unsigned char>& value) {
    auto transaction = makeTransaction();
    assert(transaction);

    std::string value_str(value.begin(), value.end());
    std::string key_str(key.begin(), key.end());
    auto put_status = transaction->Put(key_str, value_str);
    assert(put_status.ok());

    auto commit_status = transaction->Commit();
    assert(commit_status.ok());

    transaction.reset();
    return commit_status;
}

rocksdb::Status CheckpointStorage::deleteKeyValuePair(
    const std::vector<unsigned char>& key) {
    auto transaction = makeTransaction();
    assert(transaction);

    std::string key_str(key.begin(), key.end());
    auto delete_status = transaction->Delete(key_str);
    assert(delete_status.ok());

    auto commit_status = transaction->Commit();
    assert(commit_status.ok());

    transaction.reset();
    return commit_status;
}
