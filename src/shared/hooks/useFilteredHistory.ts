import { useMemo } from "react";
import type { ClipboardEntry } from "../types";
import { matchesSearch } from "../lib/searchQuery";

interface UseFilteredHistoryOptions {
  history: ClipboardEntry[];
  search: string;
  typeFilter: string | null;
}

export const useFilteredHistory = ({
  history,
  search,
  typeFilter
}: UseFilteredHistoryOptions) => {
  return useMemo(() => {
    const filtered = history.filter((item) => {
      if (typeFilter && item.content_type !== typeFilter) {
        return false;
      }
      return matchesSearch(item, search);
    });

    return filtered.sort((a, b) => {
      if (a.is_pinned !== b.is_pinned) {
        return a.is_pinned ? -1 : 1;
      }
      if (a.is_pinned) {
        if ((a.pinned_order || 0) !== (b.pinned_order || 0)) {
          return (b.pinned_order || 0) - (a.pinned_order || 0);
        }
        return b.timestamp - a.timestamp;
      }
      return b.timestamp - a.timestamp;
    });
  }, [history, search, typeFilter]);
};
