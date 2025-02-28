////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_STATIC_STRINGS_H
#define ARANGODB_BASICS_STATIC_STRINGS_H 1

#include <string>

namespace arangodb {
class StaticStrings {
  StaticStrings() = delete;

 public:
  // constants
  static std::string const Base64;
  static std::string const Binary;
  static std::string const Empty;
  static std::string const N1800;

  // index lookup strings
  static std::string const IndexEq;
  static std::string const IndexIn;
  static std::string const IndexLe;
  static std::string const IndexLt;
  static std::string const IndexGe;
  static std::string const IndexGt;

  // system attribute names
  static std::string const AttachmentString;
  static std::string const IdString;
  static std::string const KeyString;
  static std::string const RevString;
  static std::string const FromString;
  static std::string const ToString;
  static std::string const TimeString;

  // URL parameter names
  static std::string const IgnoreRevsString;
  static std::string const IsRestoreString;
  static std::string const KeepNullString;
  static std::string const MergeObjectsString;
  static std::string const ReturnNewString;
  static std::string const ReturnOldString;
  static std::string const SilentString;
  static std::string const WaitForSyncString;
  static std::string const IsSynchronousReplicationString;
  static std::string const Group;
  static std::string const Namespace;
  static std::string const ReplaceExisting;
  static std::string const Prefix;
  static std::string const OverWrite;

  // replication headers
  static std::string const ReplicationHeaderCheckMore;
  static std::string const ReplicationHeaderLastIncluded;
  static std::string const ReplicationHeaderLastScanned;
  static std::string const ReplicationHeaderLastTick;
  static std::string const ReplicationHeaderFromPresent;
  static std::string const ReplicationHeaderActive;

  // database names
  static std::string const SystemDatabase;

  // collection names
  static std::string const AnalyzersCollection;
  static std::string const LegacyAnalyzersCollection;
  static std::string const UsersCollection;
  static std::string const GraphsCollection;
  static std::string const AqlFunctionsCollection;
  static std::string const QueuesCollection;
  static std::string const JobsCollection;
  static std::string const AppsCollection;
  static std::string const AppBundlesCollection;
  static std::string const ModulesCollection;
  static std::string const FishbowlCollection;
  static std::string const FrontendCollection;
  static std::string const StatisticsCollection;
  static std::string const Statistics15Collection;
  static std::string const StatisticsRawCollection;

  // database definition fields
  static std::string const DatabaseId;
  static std::string const DatabaseName;
  static std::string const DatabaseOptions;
  static std::string const DatabaseCoordinator;
  static std::string const DatabaseCoordinatorRebootId;
  static std::string const DatabaseIsBuilding;

  // LogicalDataSource definition fields
  static std::string const DataSourceDeleted;  // data-source deletion marker
  static std::string const DataSourceGuid;     // data-source globaly-unique id
  static std::string const DataSourceId;       // data-source id
  static std::string const DataSourceName;     // data-source name
  static std::string const DataSourcePlanId;   // data-source plan id
  static std::string const DataSourceSystem;   // data-source system marker
  static std::string const DataSourceType;     // data-source type

  // Index definition fields
  static std::string const IndexExpireAfter;   // ttl index expire value
  static std::string const IndexFields;        // index fields
  static std::string const IndexId;            // index id
  static std::string const IndexInBackground;  // index in background
  static std::string const IndexIsBuilding;    // index build in-process
  static std::string const IndexName;          // index name
  static std::string const IndexSparse;        // index sparsity marker
  static std::string const IndexType;          // index type
  static std::string const IndexUnique;        // index uniqueness marker

  // static index names
  static std::string const IndexNameEdge;
  static std::string const IndexNameEdgeFrom;
  static std::string const IndexNameEdgeTo;
  static std::string const IndexNameInaccessible;
  static std::string const IndexNamePrimary;
  static std::string const IndexNameTime;

  // index hint strings
  static std::string const IndexHintAny;
  static std::string const IndexHintCollection;
  static std::string const IndexHintHint;
  static std::string const IndexHintDepth;
  static std::string const IndexHintInbound;
  static std::string const IndexHintOption;
  static std::string const IndexHintOptionForce;
  static std::string const IndexHintOutbound;
  static std::string const IndexHintWildcard;

  // HTTP headers
  static std::string const Accept;
  static std::string const AcceptEncoding;
  static std::string const AccessControlAllowCredentials;
  static std::string const AccessControlAllowHeaders;
  static std::string const AccessControlAllowMethods;
  static std::string const AccessControlAllowOrigin;
  static std::string const AccessControlExposeHeaders;
  static std::string const AccessControlMaxAge;
  static std::string const AccessControlRequestHeaders;
  static std::string const Allow;
  static std::string const AllowDirtyReads;
  static std::string const Async;
  static std::string const AsyncId;
  static std::string const Authorization;
  static std::string const BatchContentType;
  static std::string const CacheControl;
  static std::string const Close;
  static std::string const ClusterCommSource;
  static std::string const Code;
  static std::string const Connection;
  static std::string const ContentEncoding;
  static std::string const ContentLength;
  static std::string const ContentTypeHeader;
  static std::string const CorsMethods;
  static std::string const Error;
  static std::string const ErrorMessage;
  static std::string const ErrorNum;
  static std::string const Errors;
  static std::string const ErrorCodes;
  static std::string const Etag;
  static std::string const Expect;
  static std::string const ExposedCorsHeaders;
  static std::string const HLCHeader;
  static std::string const KeepAlive;
  static std::string const LeaderEndpoint;
  static std::string const Location;
  static std::string const NoSniff;
  static std::string const Origin;
  static std::string const PotentialDirtyRead;
  static std::string const RequestForwardedTo;
  static std::string const ResponseCode;
  static std::string const Server;
  static std::string const TransferEncoding;
  static std::string const TransactionBody;
  static std::string const TransactionId;
  static std::string const Unlimited;
  static std::string const WwwAuthenticate;
  static std::string const XContentTypeOptions;
  static std::string const XArangoFrontend;

  // mime types
  static std::string const MimeTypeDump;
  static std::string const MimeTypeHtml;
  static std::string const MimeTypeJson;
  static std::string const MimeTypeJsonNoEncoding;
  static std::string const MimeTypeText;
  static std::string const MimeTypeVPack;
  static std::string const MultiPartContentType;

  // encodings
  static std::string const EncodingDeflate;

  // collection attributes
  static std::string const NumberOfShards;
  static std::string const IsSmart;
  static std::string const DistributeShardsLike;
  static std::string const CacheEnabled;
  static std::string const IndexBuckets;
  static std::string const JournalSize;
  static std::string const DoCompact;
  static std::string const ReplicationFactor;
  static std::string const MinReplicationFactor;
  static std::string const ShardKeys;
  static std::string const ShardingStrategy;
  static std::string const SmartJoinAttribute;
  static std::string const Sharding;
  static std::string const Satellite;

  // graph attribute names
  static std::string const GraphCollection;
  static std::string const GraphIsSmart;
  static std::string const GraphFrom;
  static std::string const GraphTo;
  static std::string const GraphOptions;
  static std::string const GraphSmartGraphAttribute;
  static std::string const GraphDropCollections;
  static std::string const GraphDropCollection;
  static std::string const GraphCreateCollections;
  static std::string const GraphCreateCollection;
  static std::string const GraphEdgeDefinitions;
  static std::string const GraphOrphans;
  static std::string const GraphInitial;
  static std::string const GraphInitialCid;
  static std::string const GraphName;

  // Replication
  static std::string const ReplicationSoftLockOnly;
  static std::string const FailoverCandidates;

  // misc strings
  static std::string const LastValue;
  static std::string const checksumFileJs;
  static std::string const IsBuilding;
  static std::string const RebootId;
  static std::string const New;
  static std::string const Old;
  static std::string const UpgradeEnvName;
  static std::string const BackupToDeleteName;
  static std::string const BackupSearchToDeleteName;
  static std::string const SerializationFormat;
};
}  // namespace arangodb

#endif
