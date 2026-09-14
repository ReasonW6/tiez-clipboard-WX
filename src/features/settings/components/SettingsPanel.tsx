
import { memo, useState, useEffect, useCallback } from "react";
import { getVersion } from "@tauri-apps/api/app";
import { ChevronRight, HelpCircle } from "lucide-react";
import { motion } from "framer-motion";
import type { Locale } from "../../../shared/types";
import type { DefaultAppsMap, InstalledAppOption, SettingsSubpage} from "../../app/types";
import type { AppCleanupPolicy} from "../types";
import AppSelectorModal from "./AppSelectorModal";
// Removed UpdateModal imports

import GeneralSettingsGroup from "./groups/GeneralSettingsGroup";
import ClipboardSettingsGroup from "./groups/ClipboardSettingsGroup";
import AdvancedSettingsGroup from "./groups/AdvancedSettingsGroup";
import AppearanceSettingsGroup from "./groups/AppearanceSettingsGroup";

import DefaultAppsSettingsGroup from "./groups/DefaultAppsSettingsGroup";
import DataSettingsGroup from "./groups/DataSettingsGroup";

import SettingsFooter from "./SettingsFooter";

interface SettingsPanelProps {
    t: (key: string) => string;
    theme: string;
    language: Locale;
    colorMode: string;
    showSourceAppIcon: boolean;
    setShowSourceAppIcon: (val: boolean) => void;
    clipboardItemFontSize: number;
    setClipboardItemFontSize: (val: number) => void;
    clipboardTagFontSize: number;
    setClipboardTagFontSize: (val: number) => void;

    // State
    collapsedGroups: Record<string, boolean>;
    settingsSubpage: SettingsSubpage;
    autoStart: boolean;
    silentStart: boolean;
    persistent: boolean;
    persistentLimitEnabled: boolean;
    persistentLimit: number;
    deduplicate: boolean;
    captureFiles: boolean;
    captureRichText: boolean;
    richTextSnapshotPreview: boolean;
    deleteAfterPaste: boolean;
    moveToTopAfterPaste: boolean;
    sequentialMode: boolean;
    sequentialHotkey: string;
    isRecordingSequential: boolean;
    richPasteHotkey: string;
    isRecordingRich: boolean;
    searchHotkey: string;
    isRecordingSearch: boolean;
    quickPasteModifier: "disabled" | "ctrl" | "alt" | "shift" | "win";
    setQuickPasteModifier: (val: "disabled" | "ctrl" | "alt" | "shift" | "win") => void;
    privacyProtection: boolean;
    privacyProtectionKinds: string[];
    setPrivacyProtectionKinds: (val: string[]) => void;
    privacyProtectionCustomRules: string;
    setPrivacyProtectionCustomRules: (val: string) => void;
    sensitiveMaskPrefixVisible: number;
    setSensitiveMaskPrefixVisible: (val: number) => void;
    sensitiveMaskSuffixVisible: number;
    setSensitiveMaskSuffixVisible: (val: number) => void;
    sensitiveMaskEmailDomain: boolean;
    setSensitiveMaskEmailDomain: (val: boolean) => void;
    cleanupRules: string;
    setCleanupRules: (val: string) => void;
    appCleanupPolicies: AppCleanupPolicy[];
    setAppCleanupPolicies: (val: AppCleanupPolicy[]) => void;
    hotkey: string;
    showHotkeyHint: boolean;
    showSearchBox: boolean;
    setShowSearchBox: (val: boolean) => void;
    scrollTopButtonEnabled: boolean;
    setScrollTopButtonEnabled: (val: boolean) => void;
    emojiPanelEnabled: boolean;
    setEmojiPanelEnabled: (val: boolean) => void;
    tagManagerEnabled: boolean;
    setTagManagerEnabled: (val: boolean) => void;
    arrowKeySelection: boolean;
    setArrowKeySelection: (val: boolean) => void;

    soundEnabled: boolean;
    setSoundEnabled: (val: boolean) => void;
    pasteSoundEnabled: boolean;
    setPasteSoundEnabled: (val: boolean) => void;
    soundVolume: number;
    setSoundVolume: (val: number) => void;
    hideTrayIcon: boolean;
    setHideTrayIcon: (val: boolean) => void;
    hideDockIcon: boolean;
    setHideDockIcon: (val: boolean) => void;
    edgeDocking: boolean;
    setEdgeDocking: (val: boolean) => void;
    customBackground: string;
    setCustomBackground: (val: string) => void;
    customBackgroundOpacity: number;
    setCustomBackgroundOpacity: (val: number) => void;
    surfaceOpacity: number;
    setSurfaceOpacity: (val: number) => void;

    installedApps: InstalledAppOption[];
    appSettings: Record<string, string>;
    defaultApps: DefaultAppsMap;
    showAppSelector: string | null;
    dataPath: string;

    // Setters/Actions
    toggleGroup: (group: string) => void;
    setSettingsSubpage: (val: SettingsSubpage) => void;
    setAutoStart: (val: boolean) => void;
    setSilentStart: (val: boolean) => void;
    setPersistent: (val: boolean) => void;
    setPersistentLimitEnabled: (val: boolean) => void;
    setPersistentLimit: (val: number) => void;
    setDeduplicate: (val: boolean) => void;
    setCaptureFiles: (val: boolean) => void;
    setCaptureRichText: (val: boolean) => void;
    setRichTextSnapshotPreview: (val: boolean) => void;
    setDeleteAfterPaste: (val: boolean) => void;
    setMoveToTopAfterPaste: (val: boolean) => void;
    saveAppSetting: (key: string, val: string) => void;
    setSequentialModeState: (val: boolean) => void;
    setIsRecordingSequential: (val: boolean) => void;
    updateSequentialHotkey: (key: string) => void;
    setIsRecordingRich: (val: boolean) => void;
    updateRichPasteHotkey: (key: string) => void;
    setIsRecordingSearch: (val: boolean) => void;
    updateSearchHotkey: (key: string) => void;
    setPrivacyProtection: (val: boolean) => void;
    setShowHotkeyHint: (val: boolean) => void;
    setIsRecording: (val: boolean) => void;
    isRecording: boolean;
    hotkeyParts: string[];
    updateHotkey: (key: string) => void;

    setTheme: (val: string) => void;
    setColorMode: (val: string) => void;
    setLanguage: (val: Locale) => void;

    compactMode: boolean;
    setCompactMode: (val: boolean) => void;
    checkHotkeyConflict: (newHotkey: string, mode: 'main' | 'sequential' | 'rich' | 'search') => boolean;

    setShowAppSelector: (val: string | null) => void;
    handleResetSettings: () => void;


}

const SettingsPanel = (props: SettingsPanelProps) => {
    const {
        t, theme, language, colorMode, showSourceAppIcon, setShowSourceAppIcon,
        collapsedGroups, settingsSubpage, autoStart, silentStart, persistent, persistentLimitEnabled, persistentLimit, deduplicate, captureFiles, captureRichText, richTextSnapshotPreview, deleteAfterPaste, moveToTopAfterPaste,
        sequentialMode, sequentialHotkey, isRecordingSequential,
        richPasteHotkey, isRecordingRich, searchHotkey, isRecordingSearch, quickPasteModifier, setQuickPasteModifier,
        privacyProtection, privacyProtectionKinds, setPrivacyProtectionKinds, privacyProtectionCustomRules, setPrivacyProtectionCustomRules, sensitiveMaskPrefixVisible, setSensitiveMaskPrefixVisible, sensitiveMaskSuffixVisible, setSensitiveMaskSuffixVisible, sensitiveMaskEmailDomain, setSensitiveMaskEmailDomain, cleanupRules, setCleanupRules, appCleanupPolicies, setAppCleanupPolicies, showSearchBox, setShowSearchBox, scrollTopButtonEnabled, setScrollTopButtonEnabled, arrowKeySelection, setArrowKeySelection,
        soundEnabled, setSoundEnabled, pasteSoundEnabled, setPasteSoundEnabled,
        soundVolume, setSoundVolume,
        hideTrayIcon, setHideTrayIcon,
        hideDockIcon, setHideDockIcon,
        edgeDocking, setEdgeDocking,
        customBackground, setCustomBackground,
        customBackgroundOpacity, setCustomBackgroundOpacity,
        surfaceOpacity, setSurfaceOpacity,
        installedApps, appSettings, defaultApps, showAppSelector, dataPath,

        toggleGroup, setSettingsSubpage, setAutoStart, setSilentStart, setPersistent, setPersistentLimitEnabled, setPersistentLimit, setDeduplicate, setCaptureFiles, setCaptureRichText, setRichTextSnapshotPreview, setDeleteAfterPaste, setMoveToTopAfterPaste, saveAppSetting,
        setSequentialModeState, setIsRecordingSequential, updateSequentialHotkey,
        setIsRecordingRich, updateRichPasteHotkey,
        setIsRecordingSearch, updateSearchHotkey,
        setPrivacyProtection,
        setIsRecording, isRecording, hotkey, hotkeyParts, updateHotkey,
        setTheme, setColorMode, setLanguage, compactMode, setCompactMode, checkHotkeyConflict,
        clipboardItemFontSize, setClipboardItemFontSize, clipboardTagFontSize, setClipboardTagFontSize,
        emojiPanelEnabled, setEmojiPanelEnabled, tagManagerEnabled, setTagManagerEnabled,
        setShowAppSelector, handleResetSettings,
        } = props;

    const [emailCopied, setEmailCopied] = useState(false);
    const [appVersion, setAppVersion] = useState("");

    const [updateStatus, setUpdateStatus] = useState<string>("");
    // Removed updateModalData
    const [openHints, setOpenHints] = useState<Set<string>>(new Set());
    const [privacyKindsOpen, setPrivacyKindsOpen] = useState(false);
    const [privacyRulesOpen, setPrivacyRulesOpen] = useState(false);

    const toggleHint = (key: string) => {
        setOpenHints(prev => {
            const next = new Set(prev);
            if (next.has(key)) next.delete(key);
            else next.add(key);
            return next;
        });
    };

    const LabelWithHint = ({ label, hint, hintKey }: { label: string; hint?: string | React.ReactNode; hintKey: string }) => (
        <div className="item-label-group">
            <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
                <span className="item-label">{label}</span>
                {hint && (
                    <button
                        type="button"
                        className="hint-icon-btn"
                        title={typeof hint === 'string' ? hint : undefined}
                        onClick={(e) => {
                            e.stopPropagation();
                            toggleHint(hintKey);
                        }}
                    >
                        <HelpCircle size={12} />
                    </button>
                )}
            </div>
            {hint && openHints.has(hintKey) && (
                typeof hint === 'string' ? <span className="hint">{hint}</span> : hint
            )}
        </div>
    );

    useEffect(() => {
        getVersion()
            .then(v => setAppVersion(v))
            .catch(err => {
                console.error("Failed to get version:", err);
                setAppVersion("0.2.0");
            });

    }, []);

    const openAdvancedSettingsWindow = useCallback(() => {
        setSettingsSubpage("advanced");
    }, [setSettingsSubpage]);

    return (
        <motion.div
            initial={{ opacity: 0, x: 20 }}
            animate={{ opacity: 1, x: 0 }}
            style={{ display: 'flex', flexDirection: 'column', gap: '4px', minHeight: '100%', flex: 1 }}
        >
            {settingsSubpage === "advanced" ? (
                <>
                    <AdvancedSettingsGroup
                        t={t}
                        cleanupRules={cleanupRules}
                        setCleanupRules={setCleanupRules}
                        appCleanupPolicies={appCleanupPolicies}
                        setAppCleanupPolicies={setAppCleanupPolicies}
                        installedApps={installedApps}
                    />

                    <AppSelectorModal
                        show={showAppSelector}
                        installedApps={installedApps}
                        theme={theme}
                        colorMode={colorMode}
                        t={t}
                        onClose={() => setShowAppSelector(null)}
                        onSave={saveAppSetting}
                    />

                    {/* Removed UpdateModal in advanced */}
                </>
            ) : (
                <>
            {/* General Settings */}
            <GeneralSettingsGroup
                t={t}
                collapsed={collapsedGroups['general']}
                onToggle={() => toggleGroup('general')}
                LabelWithHint={LabelWithHint}
                autoStart={autoStart}
                setAutoStart={setAutoStart}
                silentStart={silentStart}
                setSilentStart={setSilentStart}
                hideTrayIcon={hideTrayIcon}
                setHideTrayIcon={setHideTrayIcon}
                hideDockIcon={hideDockIcon}
                setHideDockIcon={setHideDockIcon}
                edgeDocking={edgeDocking}
                setEdgeDocking={setEdgeDocking}
                soundEnabled={soundEnabled}
                setSoundEnabled={setSoundEnabled}
                pasteSoundEnabled={pasteSoundEnabled}
                setPasteSoundEnabled={setPasteSoundEnabled}
                soundVolume={soundVolume}
                setSoundVolume={setSoundVolume}
                showSearchBox={showSearchBox}
                setShowSearchBox={setShowSearchBox}
                scrollTopButtonEnabled={scrollTopButtonEnabled}
                setScrollTopButtonEnabled={setScrollTopButtonEnabled}
                emojiPanelEnabled={emojiPanelEnabled}
                setEmojiPanelEnabled={setEmojiPanelEnabled}
                tagManagerEnabled={tagManagerEnabled}
                setTagManagerEnabled={setTagManagerEnabled}
                arrowKeySelection={arrowKeySelection}
                setArrowKeySelection={setArrowKeySelection}
                saveAppSetting={saveAppSetting}
            />

            {/* Clipboard Settings */}
            <ClipboardSettingsGroup
                t={t}
                collapsed={collapsedGroups['clipboard']}
                onToggle={() => toggleGroup('clipboard')}
                LabelWithHint={LabelWithHint}
                persistent={persistent}
                setPersistent={setPersistent}
                persistentLimitEnabled={persistentLimitEnabled}
                setPersistentLimitEnabled={setPersistentLimitEnabled}
                persistentLimit={persistentLimit}
                setPersistentLimit={setPersistentLimit}
                saveAppSetting={saveAppSetting}
                deduplicate={deduplicate}
                setDeduplicate={setDeduplicate}
                captureFiles={captureFiles}
                setCaptureFiles={setCaptureFiles}
                captureRichText={captureRichText}
                setCaptureRichText={setCaptureRichText}
                richTextSnapshotPreview={richTextSnapshotPreview}
                setRichTextSnapshotPreview={setRichTextSnapshotPreview}
                richPasteHotkey={richPasteHotkey}
                isRecordingRich={isRecordingRich}
                setIsRecordingRich={setIsRecordingRich}
                updateRichPasteHotkey={updateRichPasteHotkey}
                searchHotkey={searchHotkey}
                isRecordingSearch={isRecordingSearch}
                setIsRecordingSearch={setIsRecordingSearch}
                updateSearchHotkey={updateSearchHotkey}
                quickPasteModifier={quickPasteModifier}
                setQuickPasteModifier={setQuickPasteModifier}
                deleteAfterPaste={deleteAfterPaste}
                setDeleteAfterPaste={setDeleteAfterPaste}
                moveToTopAfterPaste={moveToTopAfterPaste}
                setMoveToTopAfterPaste={setMoveToTopAfterPaste}
                sequentialMode={sequentialMode}
                setSequentialModeState={setSequentialModeState}
                sequentialHotkey={sequentialHotkey}
                isRecordingSequential={isRecordingSequential}
                setIsRecordingSequential={setIsRecordingSequential}
                updateSequentialHotkey={updateSequentialHotkey}
                checkHotkeyConflict={checkHotkeyConflict}
                privacyProtection={privacyProtection}
                setPrivacyProtection={setPrivacyProtection}
                privacyProtectionKinds={privacyProtectionKinds}
                setPrivacyProtectionKinds={setPrivacyProtectionKinds}
                privacyProtectionCustomRules={privacyProtectionCustomRules}
                setPrivacyProtectionCustomRules={setPrivacyProtectionCustomRules}
                sensitiveMaskPrefixVisible={sensitiveMaskPrefixVisible}
                setSensitiveMaskPrefixVisible={setSensitiveMaskPrefixVisible}
                sensitiveMaskSuffixVisible={sensitiveMaskSuffixVisible}
                setSensitiveMaskSuffixVisible={setSensitiveMaskSuffixVisible}
                sensitiveMaskEmailDomain={sensitiveMaskEmailDomain}
                setSensitiveMaskEmailDomain={setSensitiveMaskEmailDomain}
                privacyKindsOpen={privacyKindsOpen}
                setPrivacyKindsOpen={setPrivacyKindsOpen}
                privacyRulesOpen={privacyRulesOpen}
                setPrivacyRulesOpen={setPrivacyRulesOpen}

                isRecording={isRecording}
                setIsRecording={setIsRecording}
                hotkeyParts={hotkeyParts}
                updateHotkey={updateHotkey}
                hotkey={hotkey}
                appSettings={appSettings}
                theme={theme}
                colorMode={colorMode}
            />

            {/* Appearance Settings */}
            <AppearanceSettingsGroup
                t={t}
                collapsed={collapsedGroups['appearance']}
                onToggle={() => toggleGroup('appearance')}
                LabelWithHint={LabelWithHint}
                theme={theme}
                setTheme={setTheme}
                colorMode={colorMode}
                setColorMode={setColorMode}
                language={language}
                setLanguage={setLanguage}
                showSourceAppIcon={showSourceAppIcon}
                setShowSourceAppIcon={setShowSourceAppIcon}
                compactMode={compactMode}
                setCompactMode={setCompactMode}
                clipboardItemFontSize={clipboardItemFontSize}
                setClipboardItemFontSize={setClipboardItemFontSize}
                clipboardTagFontSize={clipboardTagFontSize}
                setClipboardTagFontSize={setClipboardTagFontSize}
                customBackground={customBackground}
                setCustomBackground={setCustomBackground}
                customBackgroundOpacity={customBackgroundOpacity}
                setCustomBackgroundOpacity={setCustomBackgroundOpacity}
                surfaceOpacity={surfaceOpacity}
                setSurfaceOpacity={setSurfaceOpacity}
                saveAppSetting={saveAppSetting}
            />




            {/* Default Apps Settings */}
            <DefaultAppsSettingsGroup
                t={t}
                collapsed={collapsedGroups['default_apps']}
                onToggle={() => toggleGroup('default_apps')}
                installedApps={installedApps}
                appSettings={appSettings}
                defaultApps={defaultApps}
                setShowAppSelector={setShowAppSelector}
            />

            {/* Data Management Settings */}
            <DataSettingsGroup
                t={t}
                collapsed={collapsedGroups['data']}
                onToggle={() => toggleGroup('data')}
                dataPath={dataPath}
            />

            <div className="settings-group">
                <button
                    type="button"
                    className="group-header settings-nav-card"
                    onClick={openAdvancedSettingsWindow}
                >
                    <div style={{ minWidth: 0, textAlign: "left" }}>
                        <h3 style={{ margin: 0 }}>{t("advanced_settings")}</h3>
                        <div className="settings-subpage-note">{t("advanced_settings_entry_desc")}</div>
                    </div>
                    <ChevronRight size={16} />
                </button>
            </div>

            <SettingsFooter
                t={t}
                appVersion={appVersion}
                updateStatus={updateStatus}
                setUpdateStatus={setUpdateStatus}
                // Removed setUpdateModalData
                onResetSettings={handleResetSettings}
                emailCopied={emailCopied}
                setEmailCopied={setEmailCopied}
            />

            <AppSelectorModal
                show={showAppSelector}
                installedApps={installedApps}
                theme={theme}
                colorMode={colorMode}
                t={t}
                onClose={() => setShowAppSelector(null)}
                onSave={saveAppSetting}
            />

            {/* Removed UpdateModal in generic */}
                </>
            )}
        </motion.div>
    );
};

export default memo(SettingsPanel);
