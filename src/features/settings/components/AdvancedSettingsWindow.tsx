import { useCallback, useRef } from "react";
import { translations } from "../../../locales";
import AdvancedSettingsGroup from "./groups/AdvancedSettingsGroup";
import { useAppState } from "../../app/hooks/useAppState";
import { useSettingsInit } from "../../../shared/hooks/useSettingsInit";
import { useSettingsPostInit } from "../../../shared/hooks/useSettingsPostInit";
import { useAppBootstrap } from "../../../shared/hooks/useAppBootstrap";
import { useSettingsApply } from "../../../shared/hooks/useSettingsApply";
import { useCustomBackground } from "../../../shared/hooks/useCustomBackground";

const AdvancedSettingsWindow = () => {
    const appState = useAppState();
    const {
        setAppSettings,
        setHotkey,
        setTheme,
        setColorMode,
        setCompactMode,
        language,
        setLanguage,
        customBackground,
        setCustomBackground,
        customBackgroundOpacity,
        setCustomBackgroundOpacity,
        surfaceOpacity,
        setSurfaceOpacity,
        setPersistent,
        setPersistentLimitEnabled,
        setPersistentLimit,
        setDeduplicate,
        setCaptureFiles,
        setCaptureRichText,
        setRichTextSnapshotPreview,
        setPrivacyProtection,
        setPrivacyProtectionKinds,
        setPrivacyProtectionCustomRules,
        cleanupRules,
        setCleanupRules,
        appCleanupPolicies,
        setAppCleanupPolicies,
        setSensitiveMaskPrefixVisible,
        setSensitiveMaskSuffixVisible,
        setSensitiveMaskEmailDomain,
        setSilentStart,
        setFollowMouse,
        showAppBorder,
        setShowAppBorder,
        setShowSourceAppIcon,
        setDeleteAfterPaste,
        setMoveToTopAfterPaste,
        setHideTrayIcon,
        setEdgeDocking,
        setShowSearchBox,
        setScrollTopButtonEnabled,
        setArrowKeySelection,
        setRegistryWinVEnabled,
        setSequentialHotkey,
        setRichPasteHotkey,
        setSearchHotkey,
        setQuickPasteModifier,
        setSequentialModeState,
        theme,
        colorMode,
        compactMode,
        settingsLoaded,
        clipboardItemFontSize,
        setClipboardItemFontSize,
        clipboardTagFontSize,
        setClipboardTagFontSize,
        setEmojiPanelEnabled,
        setTagManagerEnabled,
        setEmojiPanelTab,
        setEmojiFavorites,
        setAutoStart,
        setWinClipboardDisabled,
        setDefaultApps,
        setDataPath,
        setInstalledApps,
        setIsWindowPinned,
        setSettingsLoaded,
        setSoundEnabled,
        setSoundVolume,
        setPasteSoundEnabled,
        setPasteMethod,
        installedApps,
        hideDockIcon: _hideDockIcon,
        setHideDockIcon,
    } = appState;

    const tagManagerSizeRef = useRef<{ width: number; height: number } | null>(null);

    const t = useCallback((key: string) => {
        const k = key as keyof typeof translations["zh"];
        return translations[language][k] || translations["en"][k] || key;
    }, [language]);

    const settings = useSettingsInit({
        setAppSettings,
        setHotkey,
        setTheme,
        setColorMode,
        setCompactMode,
        setLanguage
    });

    useSettingsPostInit({
        settings,
        tagManagerSizeRef,
        setCustomBackground,
        setCustomBackgroundOpacity,
        setSurfaceOpacity,
        setPersistent,
        setPersistentLimitEnabled,
        setPersistentLimit,
        setDeduplicate,
        setCaptureFiles,
        setCaptureRichText,
        setRichTextSnapshotPreview,
        setPrivacyProtection,
        setPrivacyProtectionKinds,
        setPrivacyProtectionCustomRules,
        setSensitiveMaskPrefixVisible,
        setSensitiveMaskSuffixVisible,
        setSensitiveMaskEmailDomain,
        setCleanupRules,
        setAppCleanupPolicies,
        setSilentStart,
        setFollowMouse,
        setShowAppBorder,
        setShowSourceAppIcon,
        setDeleteAfterPaste,
        setMoveToTopAfterPaste,
        setHideTrayIcon,
        setEdgeDocking,
        setShowSearchBox,
        setScrollTopButtonEnabled,
        setArrowKeySelection,
        setRegistryWinVEnabled,
        setSequentialHotkey,
        setRichPasteHotkey,
        setSearchHotkey,
        setQuickPasteModifier,
        setSequentialModeState,
        setSoundEnabled,
        setSoundVolume,
        setPasteSoundEnabled,
        setPasteMethod,
        setIsWindowPinned,
        setSettingsLoaded,
        setClipboardItemFontSize,
        setClipboardTagFontSize,
        setEmojiPanelEnabled,
        setTagManagerEnabled,
        setEmojiPanelTab,
        setEmojiFavorites,
        setHideDockIcon
    });

    useAppBootstrap({
        setDataPath,
        setInstalledApps,
        setAutoStart,
        setWinClipboardDisabled,
        setDefaultApps
    });

    useSettingsApply({
        theme,
        colorMode,
        showAppBorder,
        compactMode,
        settingsLoaded,
        clipboardItemFontSize,
        clipboardTagFontSize,
        surfaceOpacity,
    });

    useCustomBackground({
        customBackground,
        customBackgroundOpacity,
        theme
    });

    return (
        <div className="advanced-settings-window-shell">
            <AdvancedSettingsGroup
                t={t}
                cleanupRules={cleanupRules}
                setCleanupRules={setCleanupRules}
                appCleanupPolicies={appCleanupPolicies}
                setAppCleanupPolicies={setAppCleanupPolicies}
                installedApps={installedApps}
            />
        </div>
    );
};

export default AdvancedSettingsWindow;
