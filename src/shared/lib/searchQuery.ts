import type { ClipboardEntry } from "../types";

// A tag picked from the dropdown travels in the search string ahead of the
// typed text. The marker cannot be typed into the input, so typed text such
// as "tag:work" stays plain text with its existing meaning.
const TAG_MARK = "\u0001";

export const searchWithTag = (tag: string, text = "") => `${TAG_MARK}${tag}${TAG_MARK}${text}`;

export const parseSearch = (search: string): { tag: string | null; text: string } => {
  if (search.startsWith(TAG_MARK)) {
    const end = search.indexOf(TAG_MARK, 1);
    if (end > 0) return { tag: search.slice(1, end), text: search.slice(end + 1) };
  }
  return { tag: null, text: search };
};

// What the backend is asked for. A picked tag narrows by tag; its typed text is
// applied to those results by matchesSearch.
export const backendSearch = (search: string): { term: string; tagOnly: boolean } => {
  const { tag, text } = parseSearch(search);
  if (tag !== null) return { term: tag, tagOnly: true };
  return text.startsWith("tag:") ? { term: text.slice(4), tagOnly: true } : { term: text, tagOnly: false };
};

export const matchesSearch = (item: ClipboardEntry, search: string) => {
  const { tag, text } = parseSearch(search);
  if (tag !== null) {
    const picked = tag.toLowerCase();
    if (!item.tags?.some(itemTag => itemTag.toLowerCase() === picked)) return false;
  }
  let term = text.toLowerCase();
  const tagOnly = term.startsWith("tag:");
  if (tagOnly) term = term.slice(4);
  if (!term) return true;
  if (tagOnly) return item.tags?.some(itemTag => itemTag.toLowerCase().includes(term)) ?? false;
  return !!(item.content?.toLowerCase().includes(term) ||
    item.source_app?.toLowerCase().includes(term) ||
    item.tags?.some(itemTag => itemTag.toLowerCase().includes(term)));
};
