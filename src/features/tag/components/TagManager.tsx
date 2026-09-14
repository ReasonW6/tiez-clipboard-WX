import { useState, useEffect, useRef, useCallback } from "react";
import { invoke, convertFileSrc } from "@tauri-apps/api/core";
import { listen, emit } from "@tauri-apps/api/event";
import { Edit2, Trash2, X, LayoutGrid, List, Plus, Search, ExternalLink, Check, Copy, Tags, ArrowUpDown } from "lucide-react";
import { getTagColor } from "../../../shared/lib/utils";
import type { ClipboardEntry } from "../../../shared/types";

interface TagManagerProps { t: (key: string) => string; theme: string }
interface TagInfo { name: string; count: number }
type Dialog = { kind: "create-tag" | "rename-tag" | "delete-tag"; tag?: string }
  | { kind: "create-item"; tag: string }
  | { kind: "edit-item"; id: number }
  | { kind: "delete-items"; ids: number[] };
const VIEW_KEY = "tiez_tag_manager_view_mode";
const isProtectedTag = (tag: string) => ["sensitive", "密码"].includes(tag.toLowerCase());
const focusWindow = () => { void invoke("activate_window_focus").catch(console.error); };

export default function TagManager({ t, theme }: TagManagerProps) {
  const [tags, setTags] = useState<TagInfo[]>([]);
  const [tagColors, setTagColors] = useState<Record<string, string>>({});
  const [tagSearch, setTagSearch] = useState("");
  const [selectedTag, setSelectedTag] = useState<string | null>(null);
  const [tagItems, setTagItems] = useState<ClipboardEntry[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [sortBy, setSortBy] = useState("time");
  const [viewMode, setViewMode] = useState<"list" | "grid">(() => {
    try { return localStorage.getItem(VIEW_KEY) === "grid" ? "grid" : "list"; }
    catch { return "list"; }
  });
  const [isManageMode, setIsManageMode] = useState(false);
  const [selectedIds, setSelectedIds] = useState<Set<number>>(new Set());
  const [dialog, setDialog] = useState<Dialog | null>(null);
  const [draft, setDraft] = useState("");
  const activeTag = useRef<string | null>(null);
  const itemRequest = useRef(0);
  const tagRequest = useRef(0);
  const alive = useRef(true);
  const busyRef = useRef(false);

  const loadItems = useCallback(async (tag: string) => {
    const request = ++itemRequest.current;
    setLoading(true);
    try {
      const items = await invoke<ClipboardEntry[]>("get_tag_items", { tag });
      if (!alive.current || request !== itemRequest.current || activeTag.current !== tag) return;
      setTagItems(items);
      setSelectedIds(previous => new Set([...previous].filter(id => items.some(item => item.id === id))));
      setError("");
    } catch (err) {
      if (alive.current && request === itemRequest.current) setError(String(err));
    } finally {
      if (alive.current && request === itemRequest.current) setLoading(false);
    }
  }, []);

  const selectTag = useCallback((tag: string) => {
    if (activeTag.current !== tag) {
      activeTag.current = tag; // Update before starting any asynchronous work.
      setSelectedTag(tag);
      setTagItems([]);
      setSelectedIds(new Set());
      setIsManageMode(false);
    }
    void loadItems(tag);
  }, [loadItems]);

  const refresh = useCallback(async () => {
    const request = ++tagRequest.current;
    try {
      const [counts, colors] = await Promise.all([
        invoke<Record<string, number>>("get_all_tags_info"),
        invoke<Record<string, string>>("get_tag_colors")
      ]);
      if (!alive.current || request !== tagRequest.current) return;
      const nextTags = Object.entries(counts).map(([name, count]) => ({ name, count }))
        .sort((a, b) => b.count - a.count || a.name.localeCompare(b.name));
      setTags(nextTags);
      setTagColors(colors);
      const next = nextTags.find(tag => tag.name === activeTag.current) || nextTags[0];
      if (next) selectTag(next.name);
      else {
        activeTag.current = null;
        ++itemRequest.current;
        setSelectedTag(null);
        setTagItems([]);
        setSelectedIds(new Set());
        setIsManageMode(false);
        setLoading(false);
      }
    } catch (err) {
      if (alive.current && request === tagRequest.current) { setError(String(err)); setLoading(false); }
    }
  }, [selectTag]);

  useEffect(() => {
    alive.current = true;
    void refresh();
    const unlisteners: (() => void)[] = [];
    let disposed = false;
    for (const event of ["clipboard-changed", "clipboard-updated", "clipboard-removed"]) {
      void listen(event, () => { if (!disposed) void refresh(); }).then(off => {
        if (disposed) off(); else unlisteners.push(off);
      }).catch(console.error);
    }
    return () => {
      disposed = true;
      alive.current = false;
      ++itemRequest.current;
      ++tagRequest.current;
      unlisteners.forEach(off => off());
    };
  }, [refresh]);

  useEffect(() => {
    try { localStorage.setItem(VIEW_KEY, viewMode); } catch { /* Keep the view usable without storage. */ }
  }, [viewMode]);

  const run = async (action: () => Promise<void>) => {
    if (busyRef.current) return;
    busyRef.current = true;
    setBusy(true);
    setError("");
    try { await action(); }
    catch (err) { if (alive.current) setError(String(err)); }
    finally { busyRef.current = false; if (alive.current) setBusy(false); }
  };
  const openDialog = (next: Dialog, value = "") => { setDraft(value); setDialog(next); };
  const createTag = async (name: string) => {
    const trimmed = name.trim();
    if (!trimmed) return;
    await invoke("create_new_tag", { tagName: trimmed });
    setTagSearch("");
    selectTag(trimmed);
    await refresh();
  };
  const saveDialog = () => run(async () => {
    if (!dialog) return;
    switch (dialog.kind) {
      case "create-tag": await createTag(draft); break;
      case "rename-tag":
        await invoke("rename_tag_globally", { oldName: dialog.tag, newName: draft.trim() });
        selectTag(draft.trim());
        break;
      case "delete-tag": await invoke("delete_tag_from_all", { tagName: dialog.tag }); break;
      case "create-item":
        await invoke("add_manual_item", { content: draft, contentType: "text", tags: [dialog.tag] });
        break;
      case "edit-item": await invoke("update_item_content", { id: dialog.id, newContent: draft }); break;
      case "delete-items":
        for (const id of dialog.ids) await invoke("delete_clipboard_entry", { id });
        setSelectedIds(new Set());
        break;
    }
    setDialog(null);
    await refresh();
    await emit("clipboard-changed");
  });
  const pasteItem = (item: ClipboardEntry) => run(async () => {
    await invoke("copy_to_clipboard", {
      id: item.id, content: item.content, contentType: item.content_type, paste: true, deleteAfterUse: false
    });
    if (alive.current) await refresh();
  });
  const toggleSelection = (id: number) => setSelectedIds(previous => {
    const next = new Set(previous);
    if (next.has(id)) next.delete(id); else next.add(id);
    return next;
  });

  const search = tagSearch.trim().toLowerCase();
  const filteredTags = tags.filter(tag => tag.name.toLowerCase().includes(search));
  const canCreate = search.length > 0 && !tags.some(tag => tag.name.toLowerCase() === search);
  const sortedItems = [...tagItems].sort((a, b) => sortBy === "count"
    ? (b.use_count || 0) - (a.use_count || 0) || b.timestamp - a.timestamp : b.timestamp - a.timestamp);
  const deleting = dialog?.kind === "delete-tag" || dialog?.kind === "delete-items";
  const dialogTitle = !dialog ? "" : deleting ? t("confirm_delete") : t({
    "create-tag": "tm_new_tag", "rename-tag": "tm_rename_tag", "create-item": "add_item", "edit-item": "edit_item"
  }[dialog.kind as "create-tag" | "rename-tag" | "create-item" | "edit-item"]);

  return <div className="themed-tag-manager">
    <aside className="tm-sidebar" aria-label={t("tags")}>
      <div className="tm-sidebar-heading"><span>{t("tags")} <span className="tm-total">{tags.length}</span></span>
        <button className="tm-icon" title={t("tm_new_tag")} aria-label={t("tm_new_tag")} onClick={() => openDialog({ kind: "create-tag" })}><Plus size={16} /></button>
      </div>
      <div className="tm-search">
        <Search size={15} aria-hidden="true" />
        <input aria-label={t("find_or_create")} placeholder={t("find_or_create")} value={tagSearch} onMouseDown={focusWindow} onFocus={focusWindow}
          onChange={event => setTagSearch(event.target.value)} onKeyDown={event => {
            if (event.key !== "Enter" || event.nativeEvent.isComposing) return;
            const exact = tags.find(tag => tag.name.toLowerCase() === search);
            if (exact) selectTag(exact.name); else if (canCreate) void run(() => createTag(tagSearch));
          }} />
        {tagSearch && <button className="tm-icon" aria-label={t("tm_clear_search")} onClick={() => setTagSearch("")}><X size={14} /></button>}
      </div>
      <nav className="tm-tags" aria-label={t("tm_choose_tag")}>
        {filteredTags.map(tag => <button key={tag.name} className={`tm-tag ${selectedTag === tag.name ? "active" : ""}`}
          aria-pressed={selectedTag === tag.name} title={tag.name} onClick={() => selectTag(tag.name)}>
          <span className="tm-dot" style={{ background: tagColors[tag.name] || getTagColor(tag.name, theme) }} />
          <span className="tm-tag-name">{tag.name}</span><span className="tm-count">{tag.count}</span>
        </button>)}
        {canCreate && <button className="tm-tag tm-create-tag" disabled={busy} onClick={() => void run(() => createTag(tagSearch))}>
          <Plus size={14} /><span className="tm-tag-name">{t("create_tag_hint").replace("{tag}", tagSearch.trim())}</span>
        </button>}
        {filteredTags.length === 0 && !canCreate && <span className="tm-muted">{t("no_tags")}</span>}
      </nav>
    </aside>
    <section className="tm-content" aria-label={selectedTag || t("tags")}>
      <div className="tm-content-heading">
        <div className="tm-current-tag">
          {selectedTag && <label className="tm-color-control" title={t("tm_tag_color")}>
            <span className="tm-dot" style={{ background: tagColors[selectedTag] || getTagColor(selectedTag, theme) }} />
            <input type="color" onMouseDown={focusWindow} aria-label={t("tm_tag_color")} value={tagColors[selectedTag] || "#4d7ce8"} disabled={busy}
              onChange={event => {
                const name = selectedTag, color = event.target.value;
                setTagColors(previous => ({ ...previous, [name]: color }));
                void run(async () => { await invoke("set_tag_color", { name, color }); await emit("tag-colors-updated"); });
              }} />
          </label>}
          <h2 title={selectedTag || ""}>{selectedTag || t("tm_library")}</h2>
          {selectedTag && !isProtectedTag(selectedTag) && <div className="tm-tag-actions">
            <button className="tm-icon" title={t("tm_rename_tag")} aria-label={t("tm_rename_tag")} onClick={() => openDialog({ kind: "rename-tag", tag: selectedTag }, selectedTag)}><Edit2 size={13} /></button>
            <button className="tm-icon" title={t("tm_delete_tag")} aria-label={t("tm_delete_tag")} onClick={() => openDialog({ kind: "delete-tag", tag: selectedTag })}><Trash2 size={13} /></button>
          </div>}
        </div>
        {selectedTag && <button className="tm-button tm-primary tm-add" onClick={() => openDialog({ kind: "create-item", tag: selectedTag })}><Plus size={15} />{t("add_item")}</button>}
      </div>
      {selectedTag && <div className="tm-toolbar">
        <label className="tm-sort"><ArrowUpDown size={13} /><select onMouseDown={focusWindow} aria-label={t("tm_sort")} value={sortBy} onChange={event => setSortBy(event.target.value)}>
          <option value="time">{t("sort_time")}</option><option value="count">{t("sort_usage")}</option>
        </select></label>
        <div className="tm-toolbar-end">
          <button className={`tm-button ${isManageMode ? "active" : ""}`} aria-pressed={isManageMode} onClick={() => { setIsManageMode(!isManageMode); setSelectedIds(new Set()); }}>
            <Check size={14} />{t(isManageMode ? "cancel" : "manage")}
          </button>
          <div className="tm-view-toggle" role="group" aria-label={t("tm_view")}>
            <button className={`tm-icon ${viewMode === "list" ? "active" : ""}`} aria-pressed={viewMode === "list"} aria-label={t("tm_list_view")} title={t("tm_list_view")} onClick={() => setViewMode("list")}><List size={15} /></button>
            <button className={`tm-icon ${viewMode === "grid" ? "active" : ""}`} aria-pressed={viewMode === "grid"} aria-label={t("tm_grid_view")} title={t("tm_grid_view")} onClick={() => setViewMode("grid")}><LayoutGrid size={15} /></button>
          </div>
        </div>
      </div>}
      {error && <div className="tm-error" role="alert"><span>{t("tm_action_failed")} {error}</span><button className="tm-icon" aria-label={t("close")} onClick={() => setError("")}><X size={14} /></button></div>}
      <div className="tm-items-scroll" aria-busy={loading}>
        {tagItems.length === 0 ? <div className="tm-empty">
          <span className="tm-empty-icon"><Tags size={27} strokeWidth={1.5} /></span>
          <strong>{loading ? t("processing") : selectedTag ? t("no_items") : t("no_tags")}</strong>
          {!loading && <p>{t(selectedTag ? "tm_empty_items_hint" : "tm_empty_tags_hint")}</p>}
          {!loading && <button className="tm-button" onClick={() => openDialog(selectedTag ? { kind: "create-item", tag: selectedTag } : { kind: "create-tag" })}><Plus size={14} />{t(selectedTag ? "add_item" : "tm_new_tag")}</button>}
        </div> : <div className={`tm-items tm-items-${viewMode}`}>
          {sortedItems.map(item => <article key={item.id} data-entry-id={item.id} className={`tm-card ${selectedIds.has(item.id) ? "selected" : ""}`}>
            <button className="tm-paste-target" disabled={busy} aria-label={`${t(isManageMode ? "tm_select" : "tm_paste")} ${item.preview || item.content}`}
              aria-pressed={isManageMode ? selectedIds.has(item.id) : undefined}
              onClick={() => isManageMode ? toggleSelection(item.id) : void pasteItem(item)}>
              {isManageMode && <span className="tm-checkbox">{selectedIds.has(item.id) && <Check size={12} />}</span>}
              {item.content_type === "image" ? <img className="tm-image" src={item.content.startsWith("data:") ? item.content : convertFileSrc(item.content)} alt={t("tm_image")} loading="lazy" />
                : <span className="tm-text">{item.preview || item.content}</span>}
            </button>
            <div className="tm-card-footer">
              <span className="tm-meta">{new Date(item.timestamp).toLocaleDateString(undefined, { month: "short", day: "numeric" })}<span>· {item.use_count || 0} {t("tm_uses")}</span></span>
              {!isManageMode && <div className="tm-card-actions">
                {["text", "code"].includes(item.content_type) && <button className="tm-icon" aria-label={t("edit_item")} title={t("edit_item")} disabled={busy} onClick={() => void run(async () => {
                  const content = await invoke<string>("get_clipboard_content", { id: item.id });
                  if (alive.current) openDialog({ kind: "edit-item", id: item.id }, content);
                })}><Edit2 size={13} /></button>}
                <button className="tm-icon" aria-label={t("open")} title={t("open")} onClick={() => void run(async () => { await invoke("open_content", { id: item.id, content: item.content, contentType: item.content_type }); })}><ExternalLink size={13} /></button>
                <button className="tm-icon tm-danger" aria-label={t("delete")} title={t("delete")} onClick={() => openDialog({ kind: "delete-items", ids: [item.id] })}><Trash2 size={13} /></button>
              </div>}
            </div>
          </article>)}
        </div>}
      </div>
      {isManageMode && <div className="tm-batch-bar">
        <span>{t("tm_selected").replace("{count}", String(selectedIds.size))}</span>
        <button className="tm-button tm-danger" disabled={busy || !selectedIds.size} onClick={() => openDialog({ kind: "delete-items", ids: [...selectedIds] })}><Trash2 size={14} />{t("delete")}</button>
        <button className="tm-button tm-primary" disabled={busy || !selectedIds.size} onClick={() => void run(async () => {
          const fullContents = await Promise.all(sortedItems.filter(item => selectedIds.has(item.id))
            .map(item => invoke<string>("get_clipboard_content", { id: item.id })));
          const content = fullContents.join("\n");
          await invoke("copy_to_clipboard", { content, contentType: "text", id: 0, paste: true, deleteAfterUse: false });
          if (alive.current) { setSelectedIds(new Set()); setIsManageMode(false); }
        })}><Copy size={14} />{t("tm_paste")}</button>
      </div>}
    </section>
    {dialog && <div className="tm-modal-backdrop" onClick={() => { if (!busy) setDialog(null); }}>
      <form className="tm-dialog" role="dialog" aria-modal="true" aria-labelledby="tm-dialog-title" onClick={event => event.stopPropagation()}
        onSubmit={event => { event.preventDefault(); void saveDialog(); }} onKeyDown={event => { if (event.key === "Escape" && !busy) { event.stopPropagation(); setDialog(null); } }}>
        <h3 id="tm-dialog-title">{dialogTitle}</h3>
        {deleting ? <p>{dialog.kind === "delete-tag" ? t("confirm_delete_tag") : t("confirm_delete_desc")}
          {dialog.kind === "delete-tag" && <strong className="tm-delete-name">{dialog.tag}</strong>}
        </p> : dialog.kind === "create-item" || dialog.kind === "edit-item" ? <textarea aria-label={t("input_content_placeholder")} placeholder={t("input_content_placeholder")} value={draft} onChange={event => setDraft(event.target.value)} onMouseDown={focusWindow} onFocus={focusWindow} autoFocus required />
          : <input aria-label={t("tags")} value={draft} onChange={event => setDraft(event.target.value)} onMouseDown={focusWindow} onFocus={focusWindow} autoFocus required />}
        {error && <p role="alert" className="tm-danger">{t("tm_action_failed")} {error}</p>}
        <div className="tm-dialog-actions"><button type="button" className="tm-button" disabled={busy} onClick={() => setDialog(null)}>{t("cancel")}</button>
          <button type="submit" className={`tm-button ${deleting ? "tm-danger" : "tm-primary"}`} disabled={busy || (!deleting && !draft.trim())}>{t(busy ? "processing" : deleting ? "delete" : "save")}</button>
        </div>
      </form>
    </div>}
  </div>;
}
