/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/shard_util.h"

#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace shardutil {

StatusWith<long long> retrieveTotalShardSize(OperationContext* txn, const ShardId& shardId) {
    auto shardRegistry = Grid::get(txn)->shardRegistry();
    auto listDatabasesStatus = shardRegistry->runIdempotentCommandOnShard(
        txn,
        shardId,
        ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
        "admin",
        BSON("listDatabases" << 1));
    if (!listDatabasesStatus.isOK()) {
        return listDatabasesStatus.getStatus();
    }

    BSONElement totalSizeElem = listDatabasesStatus.getValue()["totalSize"];
    if (!totalSizeElem.isNumber()) {
        return {ErrorCodes::NoSuchKey, "totalSize field not found in listDatabases"};
    }

    return totalSizeElem.numberLong();
}

StatusWith<BSONObj> selectMedianKey(OperationContext* txn,
                                    const ShardId& shardId,
                                    const NamespaceString& nss,
                                    const ShardKeyPattern& shardKeyPattern,
                                    const BSONObj& minKey,
                                    const BSONObj& maxKey) {
    BSONObjBuilder cmd;
    cmd.append("splitVector", nss.ns());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    cmd.append("min", minKey);
    cmd.append("max", maxKey);
    cmd.appendBool("force", true);

    auto shardRegistry = Grid::get(txn)->shardRegistry();
    auto cmdStatus = shardRegistry->runIdempotentCommandOnShard(
        txn, shardId, ReadPreferenceSetting{ReadPreference::PrimaryPreferred}, "admin", cmd.obj());
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    const auto response = std::move(cmdStatus.getValue());

    Status status = getStatusFromCommandResult(response);
    if (!status.isOK()) {
        return status;
    }

    BSONObjIterator it(response.getObjectField("splitKeys"));
    if (it.more()) {
        return it.next().Obj().getOwned();
    }

    return BSONObj();
}

StatusWith<std::vector<BSONObj>> selectChunkSplitPoints(OperationContext* txn,
                                                        const ShardId& shardId,
                                                        const NamespaceString& nss,
                                                        const ShardKeyPattern& shardKeyPattern,
                                                        const BSONObj& minKey,
                                                        const BSONObj& maxKey,
                                                        long long chunkSizeBytes,
                                                        int maxPoints,
                                                        int maxObjs) {
    BSONObjBuilder cmd;
    cmd.append("splitVector", nss.ns());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    cmd.append("min", minKey);
    cmd.append("max", maxKey);
    cmd.append("maxChunkSizeBytes", chunkSizeBytes);
    cmd.append("maxSplitPoints", maxPoints);
    cmd.append("maxChunkObjects", maxObjs);

    auto shardRegistry = Grid::get(txn)->shardRegistry();
    auto cmdStatus = shardRegistry->runIdempotentCommandOnShard(
        txn, shardId, ReadPreferenceSetting{ReadPreference::PrimaryPreferred}, "admin", cmd.obj());
    if (!cmdStatus.isOK()) {
        return cmdStatus.getStatus();
    }

    const auto response = std::move(cmdStatus.getValue());

    Status status = getStatusFromCommandResult(response);
    if (!status.isOK()) {
        return status;
    }

    std::vector<BSONObj> splitPoints;

    BSONObjIterator it(response.getObjectField("splitKeys"));
    while (it.more()) {
        splitPoints.push_back(it.next().Obj().getOwned());
    }

    return std::move(splitPoints);
}

}  // namespace shardutil
}  // namespace mongo
