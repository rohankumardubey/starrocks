// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.sql.optimizer.base;

import com.google.common.collect.Lists;
import com.starrocks.thrift.TDistributionType;

import java.util.List;

public class DistributionSpec {
    protected final DistributionType type;
    protected PropertyInfo propertyInfo;

    protected DistributionSpec(DistributionType type) {
        this(type, new PropertyInfo());
    }

    protected DistributionSpec(DistributionType type, PropertyInfo propertyInfo) {
        this.type = type;
        this.propertyInfo = propertyInfo;
    }

    // Property information for hash distribution desc, it used for check DistributionSpec satisfy condition.
    public static class PropertyInfo {
        public long tableId = -1;
        public boolean isReplicate = false;
        public List<Long> partitionIds = Lists.newArrayList();
        // record nullable columns generated by outer join
        public ColumnRefSet nullableColumns = new ColumnRefSet();

        public boolean isSinglePartition() {
            return partitionIds.size() == 1;
        }

        public boolean isEmptyPartition() {
            return partitionIds.size() == 0;
        }
    }

    public PropertyInfo getPropertyInfo() {
        return propertyInfo;
    }

    public DistributionType getType() {
        return type;
    }

    public static DistributionSpec createAnyDistributionSpec() {
        return new AnyDistributionSpec();
    }

    public static HashDistributionSpec createHashDistributionSpec(HashDistributionDesc distributionDesc) {
        return new HashDistributionSpec(distributionDesc);
    }

    public static DistributionSpec createReplicatedDistributionSpec() {
        return new ReplicatedDistributionSpec();
    }

    public static DistributionSpec createGatherDistributionSpec() {
        return new GatherDistributionSpec();
    }

    public static DistributionSpec createGatherDistributionSpec(long limit) {
        return new GatherDistributionSpec(limit);
    }

    public boolean isSatisfy(DistributionSpec spec) {
        return false;
    }

    public enum DistributionType {
        ANY,
        BROADCAST,
        SHUFFLE,
        GATHER,
        ;

        public TDistributionType toThrift() {
            if (this == ANY) {
                return TDistributionType.ANY;
            } else if (this == BROADCAST) {
                return TDistributionType.BROADCAST;
            } else if (this == SHUFFLE) {
                return TDistributionType.SHUFFLE;
            } else {
                return TDistributionType.GATHER;
            }
        }
    }

    @Override
    public String toString() {
        return type.toString();
    }
}
