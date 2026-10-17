import { useCallback, useRef, useState, type RefObject } from "react";
import { AnimatePresence, motion } from "framer-motion";
import {
  ChevronLeft,
  Pin,
  PinOff,
  Search,
  Settings as SettingsIcon,
  Smile,
  Tag,
  Trash2,
  X
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { getTagColor, getTagTextColor } from "../../../shared/lib/utils";
import { parseSearch, searchWithTag } from "../../../shared/lib/searchQuery";
import ClipboardTypeFilter from "./ClipboardTypeFilter";

// Release the inline transform after projection so each theme keeps its CSS hover motion.
const buttonTransform = (_values: unknown, transform: string) => transform === "none" ? "" : transform;

interface AppHeaderProps {
  t: (key: string) => string;
  showSettings: boolean;
  setShowSettings: (val: boolean) => void;
  showTagManager: boolean;
  setShowTagManager: (val: boolean) => void;
  tagManagerEnabled: boolean;
  showEmojiPanel: boolean;
  setShowEmojiPanel: (val: boolean) => void;
  emojiPanelEnabled: boolean;

  isWindowPinned: boolean;
  setIsWindowPinned: (val: boolean) => void;
  clearHistory: () => void;
  showSearchBox: boolean;
  search: string;
  setSearch: (val: string) => void;
  setIsComposing: (val: boolean) => void;
  searchInputRef: RefObject<HTMLInputElement | null>;
  showTagFilter: boolean;
  setShowTagFilter: (val: boolean) => void;
  allTags: string[];
  searchIsFocused: boolean;
  setSearchIsFocused: (val: boolean) => void;
  setEditingTagsId: (val: number | null) => void;
  theme: string;
  colorMode: string;
  settingsTitle: string;
  typeFilter: string | null;
  setTypeFilter: (val: string | null) => void;
  onBack: () => void;

}

const AppHeader = ({
  t,
  showSettings,
  setShowSettings,
  showTagManager,
  setShowTagManager,
  tagManagerEnabled,
  showEmojiPanel,
  setShowEmojiPanel,
  emojiPanelEnabled,
  isWindowPinned,
  setIsWindowPinned,
  clearHistory,
  showSearchBox,
  search,
  setSearch,
  setIsComposing,
  searchInputRef,
  showTagFilter,
  setShowTagFilter,
  allTags,
  searchIsFocused,
  setSearchIsFocused,
  setEditingTagsId,
  theme,
  colorMode,
  settingsTitle,
  typeFilter,
  setTypeFilter,
  onBack
}: AppHeaderProps) => {
  const secondaryActionsVisible = !showSettings && !showTagManager && !showEmojiPanel;
  const { tag: searchTag, text: searchText } = parseSearch(search);
  const [searchTagWidth, setSearchTagWidth] = useState(0);
  const searchTagObserver = useRef<ResizeObserver | null>(null);
  // The input text starts after the picked tag, whatever its rendered width.
  const observeSearchTag = useCallback((chip: HTMLSpanElement | null) => {
    searchTagObserver.current?.disconnect();
    searchTagObserver.current = null;
    if (!chip) return;
    const observer = new ResizeObserver(() => setSearchTagWidth(chip.offsetWidth));
    observer.observe(chip);
    searchTagObserver.current = observer;
  }, []);
  const tagChipStyle = (tag: string) => {
    const background = getTagColor(tag, theme);
    return { background, color: getTagTextColor(background, theme),
      filter: ["mica", "acrylic"].includes(theme) ? "none" : undefined };
  };
  const updateSearchText = (text: string) => setSearch(searchTag !== null ? searchWithTag(searchTag, text) : text);
  // Retain layout snapshots while these actions are absent from the row. Newly
  // mounted buttons otherwise start at full spacing inside a shrinking parent.
  const secondaryActionProps = {
    layout: "position" as const,
    transformTemplate: buttonTransform,
    disabled: !secondaryActionsVisible,
    "aria-hidden": !secondaryActionsVisible || undefined,
    tabIndex: secondaryActionsVisible ? undefined : -1,
    animate: { opacity: secondaryActionsVisible ? 1 : 0 },
    transition: { opacity: { duration: .12 } }
  };
  const getTypeName = (type: string) => {
    switch (type) {
      case "code": return t('type_code');
      case "link":
      case "url": return t('type_url');
      case "file": return t('type_file');
      case "image": return t('type_image');
      case "video": return t('type_video');
      case "rich_text": return t('type_rich_text');
      default: return t('type_text') || 'Text';
    }
  };

  return (
  <header className="window-drag-region">
    <div className="header-top">
      <div className="header-leading">
        {(showSettings || showTagManager || showEmojiPanel) && (
          <button className="btn-icon header-back window-no-drag" aria-label={t('back')} onClick={onBack}>
            <ChevronLeft size={18} />
          </button>
        )}
        <div className="header-drag-region" data-tauri-drag-region>
          <span className="header-title">
            {showEmojiPanel
              ? (t('emoji_panel') || '表情包')
              : showTagManager && tagManagerEnabled
                ? (t('tag_manager') || '标签管理')
                : showSettings
                  ? settingsTitle
                  : t('app_name')}
          </span>
        </div>
      </div>
      <motion.div layout className="header-actions window-no-drag">
        {/* Pin Button - Always visible but single instance */}
        <motion.button layout="position" transformTemplate={buttonTransform}
          className={`btn-icon ${isWindowPinned ? 'active' : ''}`}
          title={t('pin')}
          onClick={() => {
            const newVal = !isWindowPinned;
            setIsWindowPinned(newVal);
            invoke("set_window_pinned", { pinned: newVal }).catch(console.error);
          }}
        >
          {isWindowPinned ? <PinOff size={16} /> : <Pin size={16} />}
        </motion.button>

        <motion.button {...secondaryActionProps} className="btn-icon" title={t('clear_history')} onClick={clearHistory}>
          <Trash2 size={16} />
        </motion.button>
        {tagManagerEnabled && (
          <motion.button {...secondaryActionProps} className="btn-icon" title={t('tag_manager') || '标签管理'} onClick={() => setShowTagManager(true)}>
            <Tag size={16} />
          </motion.button>
        )}
        {emojiPanelEnabled && (
          <motion.button {...secondaryActionProps} className="btn-icon" title={t('emoji_panel') || '表情包'} onClick={() => setShowEmojiPanel(true)}>
            <Smile size={16} />
          </motion.button>
        )}
        <motion.button {...secondaryActionProps} className="btn-icon" title={t('settings')} onClick={() => setShowSettings(true)}>
          <SettingsIcon size={16} />
        </motion.button>

        <motion.button layout="position" transformTemplate={buttonTransform} className="btn-icon" title={t('hide')} onClick={async () => {
          invoke("hide_window_cmd").catch(console.error);
        }}>
          <X size={16} />
        </motion.button>
      </motion.div>
    </div>

    {!showSettings && !showTagManager && !showEmojiPanel && (
      <AnimatePresence initial={false}>
        {(showSearchBox || search.trim().length > 0) && (
          <motion.div
            initial={{ height: 0, opacity: 0, overflow: 'hidden' }}
            animate={{
              height: "auto",
              opacity: 1,
              transitionEnd: { overflow: "visible" }
            }}
            exit={{ height: 0, opacity: 0, overflow: 'hidden' }}
            transition={{ duration: 0.2, ease: "circOut" }}
            style={{ flexShrink: 0 }}
          >
            <div className="search-container window-no-drag">
              <div style={{ position: 'relative' }}>
                <Search size={14} className="search-icon" />
                {searchTag !== null && (
                  <span ref={observeSearchTag} className="tag-chip search-tag-chip" data-tag={searchTag} style={tagChipStyle(searchTag)}>
                    {searchTag}
                  </span>
                )}
                <input
                  ref={searchInputRef}
                  type="text"
                  className={`search-input ${showTagFilter && allTags.length > 0 ? 'dropdown-open' : ''}`}
                  placeholder={searchTag !== null ? "" : t('search_placeholder')}
                  value={searchText}
                  onCompositionStart={() => setIsComposing(true)}
                  onCompositionEnd={(e) => {
                    setIsComposing(false);
                    updateSearchText((e.target as HTMLInputElement).value);
                  }}
                  onChange={(e) => {
                    updateSearchText(e.target.value);
                  }}
                  onKeyDown={(e) => {
                    // Backspace into the picked tag turns it back into editable text.
                    if (e.key === "Backspace" && searchTag !== null && !searchText && !e.nativeEvent.isComposing) {
                      e.preventDefault();
                      setSearch("tag:" + searchTag);
                    }
                  }}
                  onMouseDown={() => {
                    invoke("activate_window_focus").catch(console.error);
                  }}
                  onClick={() => { setShowTagFilter(true); setEditingTagsId(null); }}
                  onFocus={() => {
                    invoke("activate_window_focus").catch(console.error);
                    setShowTagFilter(true);
                    setSearchIsFocused(true);
                    setEditingTagsId(null);
                  }}
                  onBlur={() => {
                    setTimeout(() => {
                      setShowTagFilter(false);
                      setSearchIsFocused(false);
                    }, 200);
                  }}
                  style={{ color: colorMode === 'dark' ? '#ffffff' : undefined,
                    paddingLeft: searchTag !== null ? `${32 + searchTagWidth + 6}px` : undefined }}
                />
                {showTagFilter && searchIsFocused && allTags.length > 0 && (
                  <div className="tags-dropdown">
                    <div className="tags-label">{t('tags') || "Tags"}</div>
                    <div className="tags-list">
                      {allTags.map(tag => (
                        <span
                          className="tag-chip"
                          key={tag}
                          onMouseDown={(e) => {
                            e.preventDefault();
                            // Typed text keeps searching inside the picked tag.
                            setSearch(searchWithTag(tag, searchText.startsWith("tag:") ? "" : searchText));
                            setShowTagFilter(false);
                          }}
                          data-tag={tag}
                          style={tagChipStyle(tag)}
                        >
                          {tag}
                        </span>
                      ))}
                    </div>
                  </div>
                )}
              </div>
              <ClipboardTypeFilter value={typeFilter} onChange={setTypeFilter} label={getTypeName} />

            </div>
          </motion.div>
        )}
      </AnimatePresence>
    )}
  </header>
);
};

export default AppHeader;
