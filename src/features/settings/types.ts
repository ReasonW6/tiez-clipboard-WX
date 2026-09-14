export interface UpdateModalData {
  version: string;
  notes: string;
  downloadUrl: string;
}

export type AppCleanupPolicyAction = "ignore" | "clean";

export interface AppCleanupPolicy {
  id: string;
  enabled: boolean;
  appName: string;
  appPath: string;
  action: AppCleanupPolicyAction;
  contentTypes: string[];
  cleanupRules: string;
}
