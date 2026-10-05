import type { Locale } from "../types";

const SENSITIVE_MASK = "...";
const URL_PROTOCOL_RE = /^([a-zA-Z][a-zA-Z0-9+\-.]*:\/\/)/;
const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

interface MaskOptions {
  prefixVisible: number;
  suffixVisible: number;
  maskEmailDomain: boolean;
}

const DEFAULT_MASK_OPTIONS: MaskOptions = {
  prefixVisible: 3,
  suffixVisible: 3,
  maskEmailDomain: false,
};

const MIN_MASKED_CHARS = 2;

const maskMiddleChars = (value: string, prefixVisibleCount: number, suffixVisibleCount: number) => {
  const chars = Array.from(value);
  if (chars.length <= MIN_MASKED_CHARS) {
    return SENSITIVE_MASK;
  }

  const available = chars.length - MIN_MASKED_CHARS;
  const totalRequested = prefixVisibleCount + suffixVisibleCount;
  let prefix: number, suffix: number;
  if (totalRequested <= available) {
    prefix = prefixVisibleCount;
    suffix = suffixVisibleCount;
  } else {
    prefix = totalRequested > 0 ? Math.floor(available * prefixVisibleCount / totalRequested) : 0;
    suffix = Math.min(suffixVisibleCount, available - prefix);
  }

  return `${chars.slice(0, prefix).join("")}${SENSITIVE_MASK}${chars.slice(chars.length - suffix).join("")}`;
};

// Helper function to generate a consistent color from a string based on theme
export const getTagColor = (tag: string, theme: string) => {
  let hash = 0;
  for (let i = 0; i < tag.length; i++) {
    hash = tag.charCodeAt(i) + ((hash << 5) - hash);
  }

  // Use a larger prime and multiple rotations to ensure strings that are similar
  // (like "tag1" and "tag2") produce very different hues.
  const hue = Math.abs((hash * 137.508 + (hash >> 3)) % 360);

  if (theme === "retro") {
    // Retro: Keep mechanical saturation, but avoid overly dark chips.
    return `hsl(${hue}, 58%, 48%)`;
  } else {
    // Modern: Slightly lighter to keep tag chips readable.
    return `hsl(${hue}, 76%, 62%)`;
  }
};

export const getTagTextColor = (backgroundColor: string, theme?: string) => {
  if (theme !== "mica" && theme !== "acrylic" && theme !== "liquid-glass") return "#ffffff";

  // Tags use opaque HEX from the color picker or the HSL generated above.
  // Other CSS colors can depend on the backdrop; retain their existing ink.
  const color = backgroundColor.trim();
  const hex = color.match(/^#([\da-f]{3}|[\da-f]{6})$/i);
  const hsl = color.match(/^hsl\(\s*([\d.]+)\s*,\s*([\d.]+)%\s*,\s*([\d.]+)%\s*\)$/i);
  let channels: number[];
  if (hex) {
    const digits = hex[1].length === 3 ? [...hex[1]].map(digit => digit + digit).join("") : hex[1];
    channels = [0, 2, 4].map(offset => parseInt(digits.slice(offset, offset + 2), 16) / 255);
  } else if (hsl) {
    const [hue, saturation, lightness] = hsl.slice(1).map(Number);
    if (![hue, saturation, lightness].every(Number.isFinite) || saturation > 100 || lightness > 100) return "#ffffff";
    const light = lightness / 100;
    const amplitude = saturation / 100 * Math.min(light, 1 - light);
    channels = [0, 8, 4].map(offset => {
      const phase = (offset + hue / 30) % 12;
      return light - amplitude * Math.max(-1, Math.min(phase - 3, 9 - phase, 1));
    });
  } else {
    return "#ffffff";
  }

  const luminance = channels
    .map(channel => channel <= .04045 ? channel / 12.92 : ((channel + .055) / 1.055) ** 2.4)
    .reduce((sum, channel, i) => sum + channel * [.2126, .7152, .0722][i], 0);
  // The greater black/white contrast is at least 4.58:1 on an opaque sRGB fill.
  return (luminance + .05) / .05 >= 1.05 / (luminance + .05) ? "#000000" : "#ffffff";
};

export const getConciseTime = (timestamp: number, language: Locale) => {
  const diff = Date.now() - timestamp;
  const seconds = Math.floor(diff / 1000);

  if (language === "zh") {
    if (seconds < 60) return "< 1分钟";
    if (seconds < 3600) return `${Math.floor(seconds / 60)}分钟前`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)}小时前`;
    return `${Math.floor(seconds / 86400)}天前`;
  } else {
    if (seconds < 60) return "< 1m";
    if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
    return `${Math.floor(seconds / 86400)}d ago`;
  }
};

export const formatSensitivePreview = (
  content: string,
  contentType: string,
  options?: Partial<MaskOptions>
) => {
  if (!content) return "";

  const opts = { ...DEFAULT_MASK_OPTIONS, ...options };

  if (contentType === "url") {
    const protocolMatch = content.match(URL_PROTOCOL_RE);
    if (!protocolMatch) {
      return maskMiddleChars(content, opts.prefixVisible, opts.suffixVisible);
    }

    const protocol = protocolMatch[0];
    const rest = content.slice(protocol.length);
    const maskedRest = maskMiddleChars(rest, opts.prefixVisible, opts.suffixVisible);
    return `${protocol}${maskedRest}`;
  }

  if (EMAIL_RE.test(content.trim())) {
    const email = content.trim();
    const atIndex = email.indexOf("@");
    const localPart = email.slice(0, atIndex);
    const domainPart = email.slice(atIndex + 1);

    const maskedLocal = maskMiddleChars(localPart, opts.prefixVisible, opts.suffixVisible);
    if (opts.maskEmailDomain) {
      const maskedDomain = maskMiddleChars(domainPart, opts.prefixVisible, opts.suffixVisible);
      return `${maskedLocal}@${maskedDomain}`;
    }
    return `${maskedLocal}@${domainPart}`;
  }

  return maskMiddleChars(content, opts.prefixVisible, opts.suffixVisible);
};
