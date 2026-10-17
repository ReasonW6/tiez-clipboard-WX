import { describe, expect, it } from "vitest";
import type { ClipboardEntry } from "../types";
import { backendSearch, matchesSearch, parseSearch, searchWithTag } from "./searchQuery";

const entry = (content: string, tags: string[]): ClipboardEntry => ({
  id: 1, content_type: "text", content, source_app: "Editor", timestamp: 0, preview: content,
  is_pinned: false, tags, use_count: 0, is_external: false, pinned_order: 0
} as ClipboardEntry);

describe("search with a picked tag", () => {
  it("keeps the picked tag apart from the typed text", () => {
    expect(parseSearch(searchWithTag("sensitive", "abc"))).toEqual({ tag: "sensitive", text: "abc" });
    expect(parseSearch(searchWithTag("my tag"))).toEqual({ tag: "my tag", text: "" });
    expect(parseSearch("tag:work")).toEqual({ tag: null, text: "tag:work" });
  });

  it("asks the backend for the tag and filters its text locally", () => {
    expect(backendSearch(searchWithTag("work", "report"))).toEqual({ term: "work", tagOnly: true });
    expect(backendSearch("tag:wo")).toEqual({ term: "wo", tagOnly: true });
    expect(backendSearch("report")).toEqual({ term: "report", tagOnly: false });
  });

  it("matches the picked tag exactly and the typed text inside it", () => {
    const tagged = entry("Quarterly report", ["Work"]);
    const similar = entry("Quarterly report", ["Workshop"]);
    expect(matchesSearch(tagged, searchWithTag("work"))).toBe(true);
    expect(matchesSearch(similar, searchWithTag("work"))).toBe(false);
    expect(matchesSearch(tagged, searchWithTag("work", "REPORT"))).toBe(true);
    expect(matchesSearch(tagged, searchWithTag("work", "invoice"))).toBe(false);
  });

  it("keeps the meaning of typed tag searches", () => {
    const tagged = entry("Quarterly report", ["Workshop"]);
    expect(matchesSearch(tagged, "tag:work")).toBe(true);
    expect(matchesSearch(tagged, "tag:")).toBe(true);
    expect(matchesSearch(tagged, "quarterly")).toBe(true);
    expect(matchesSearch(tagged, "editor")).toBe(true);
    expect(matchesSearch(tagged, "invoice")).toBe(false);
  });
});
