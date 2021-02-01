package com.pinterest.rocksplicator.utils;

import org.apache.helix.model.ExternalView;

public class ExternalViewUtils {
  private ExternalViewUtils() {
    throw new AssertionError(
        "Cannot instantiate static utility class ExternalViewUtils");
  }

  public static class StateUtils {
    public static boolean isStateOnline(String state) {
      return "ONLINE".equalsIgnoreCase(state);
    }

    public static boolean isStateOffline(String state) {
      return "OFFLINE".equalsIgnoreCase(state);
    }

    public static boolean isStateMaster(String state) {
      return "MASTER".equalsIgnoreCase(state);
    }

    public static boolean isStateSlave(String state) {
      return "SLAVE".equalsIgnoreCase(state);
    }

    public static boolean isStateLeader(String state) {
      return "LEADER".equalsIgnoreCase(state);
    }

    public static boolean isStateFollower(String state) {
      return "FOLLOWER".equalsIgnoreCase(state);
    }


    public static boolean isStateAnyKindOfLeader(String state) {
      return isStateLeader(state) || isStateMaster(state);
    }

    public static boolean isStateAnyKindOfFollower(String state) {
      return isStateFollower(state) || isStateSlave(state);
    }
  }

  // Only ONLINE, MASTER, LEADER, FOLLOWER and SLAVE states are ready for serving traffic
  public static boolean isServing(String state) {
    if (StateUtils.isStateOnline(state)
        || StateUtils.isStateMaster(state)
        || StateUtils.isStateLeader(state)
        || StateUtils.isStateSlave(state)
        || StateUtils.isStateFollower(state)) {
      return true;
    } else {
      return false;
    }
  }

  public static String getShortHandState(String state) {
    if (StateUtils.isStateAnyKindOfFollower(state)) {
      return ":S";
    } else if (StateUtils.isStateAnyKindOfLeader(state)) {
      return ":M";
    }
    return "";
  }

  public static boolean isMasterSlaveStateModel(ExternalView externalView) {
    return "MasterSlave".equalsIgnoreCase(externalView.getStateModelDefRef());
  }

  public static boolean isLeaderFollowerStateModel(ExternalView externalView) {
    return "LeaderFollower".equalsIgnoreCase(externalView.getStateModelDefRef());
  }

  public static boolean isReadWriteStateModel(ExternalView externalView) {
    return isMasterSlaveStateModel(externalView) || isLeaderFollowerStateModel(externalView);
  }

  public static int getNumPartitions(ExternalView externalView) {
    return Integer.parseInt(externalView.getRecord().getSimpleField("NUM_PARTITIONS"));
  }
}
