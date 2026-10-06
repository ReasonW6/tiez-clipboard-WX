import { useEffect, useLayoutEffect, useRef, useState } from "react";
import DOMPurify, { type Config } from "dompurify";
import contentStyles from "../../styles/components/rich-text-content.css?raw";
import { toTauriLocalImageSrc } from "../lib/localImageSrc";
import {
  isHtmlishTagText,
  isOfficeStyleDefinitionText,
  stripOfficePreviewNoise
} from "../lib/repairHtmlFragment";

const pickFirstSrcFromSrcset = (srcset?: string | null): string | null => {
  if (!srcset) return null;
  const first = srcset
    .split(",")
    .map((part) => part.trim())
    .find(Boolean);
  if (!first) return null;
  const url = first.split(/\s+/)[0]?.trim();
  return url || null;
};

const resolveImgSource = (el: Element): string | null => {
  const src = el.getAttribute("src")?.trim() || "";
  const lazyAttrs = [
    "data-src",
    "data-original",
    "data-lazy-src",
    "data-actualsrc",
    "data-url"
  ];
  const lazySrc =
    lazyAttrs
      .map((name) => el.getAttribute(name)?.trim() || "")
      .find(Boolean) || "";
  const srcsetPrimary = pickFirstSrcFromSrcset(el.getAttribute("srcset"));
  const lazySrcsetPrimary = pickFirstSrcFromSrcset(el.getAttribute("data-srcset"));

  const isMissingOrPlaceholder =
    !src ||
    src === "about:blank" ||
    src === "#" ||
    src.toLowerCase().startsWith("blob:");

  if (isMissingOrPlaceholder) {
    return lazySrc || srcsetPrimary || lazySrcsetPrimary || null;
  }
  return src;
};

const PURIFY_CONFIG: Config = {
  // Keep leading <style> blocks (Excel/WPS formatting) instead of dropping them with <head>.
  FORCE_BODY: true,
  // Forms and <base> could submit to or retarget the app window.
  FORBID_TAGS: ["form", "input", "button", "textarea", "select", "base"]
};

const sanitizeHTML = (html: string, preview?: boolean) => {
  const parser = new DOMParser();

  const stripLeadingPreviewNoise = (container: HTMLElement) => {
    if (!container.querySelector("table, p, div, img, ul, ol, blockquote, pre")) return;

    for (const node of Array.from(container.childNodes)) {
      if (node.nodeType === Node.TEXT_NODE) {
        const text = node.textContent?.trim() || "";
        if (!text) {
          container.removeChild(node);
          continue;
        }
        if (isHtmlishTagText(text) || isOfficeStyleDefinitionText(text)) {
          container.removeChild(node);
          continue;
        }
      }

      if (node.nodeType === Node.ELEMENT_NODE) {
        const element = node as Element;
        const tagName = element.tagName.toLowerCase();
        if (tagName === "style" && isOfficeStyleDefinitionText(element.textContent || "")) {
          container.removeChild(node);
          continue;
        }
        if (tagName === "meta" || tagName === "link" || tagName === "xml") {
          container.removeChild(node);
          continue;
        }
      }

      break;
    }
  };

  // Heuristic: If it contains table elements but no <table> tag, wrap it.
  let processedHtml = stripOfficePreviewNoise(html);
  if ((processedHtml.includes("<tr") || processedHtml.includes("<td") || processedHtml.includes("<col"))
    && !processedHtml.toLowerCase().includes("<table")) {
    processedHtml = `<table style="border-collapse: collapse; min-width: 100%;">${processedHtml}</table>`;
  }

  const doc = parser.parseFromString(processedHtml, "text/html");

  // Only remove scripts, keep styles for formatting (e.g. Excel)
  doc.querySelectorAll("script").forEach(el => el.remove());
  doc.querySelectorAll("style").forEach((style) => {
    if (isOfficeStyleDefinitionText(style.textContent || "")) {
      style.remove();
    }
  });
  doc.querySelectorAll("meta, link, xml").forEach((el) => el.remove());

  // Truncate tables for preview to save performance
  if (preview) {
    doc.querySelectorAll("table").forEach(table => {
      const rows = table.querySelectorAll("tr");
      if (rows.length > 5) {
        // Keep first 3 rows
        for (let i = 4; i < rows.length; i++) {
          rows[i].remove();
        }
        // Add a "..." indicator
        const moreRow = doc.createElement("tr");
        const moreCell = doc.createElement("td");
        moreCell.colSpan = 10;
        moreCell.style.textAlign = "center";
        moreCell.style.fontSize = "10px";
        moreCell.style.opacity = "0.5";
        moreCell.innerText = "... content truncated for preview ...";
        moreRow.appendChild(moreCell);
        table.appendChild(moreRow);
      }
    });
  }

  // Move styles from head to body to ensure they are included in the final innerHTML
  doc.head.querySelectorAll("style").forEach(style => {
    doc.body.prepend(style);
  });
  stripLeadingPreviewNoise(doc.body);

  // Handle local file images (including encoded file:// paths)
  doc.querySelectorAll("img").forEach(el => {
    const resolvedSrc = resolveImgSource(el);
    if (resolvedSrc) {
      if (el.getAttribute("src") !== resolvedSrc) {
        el.setAttribute("src", resolvedSrc);
      }
      const normalizedSrc = resolvedSrc.startsWith("//") ? `https:${resolvedSrc}` : resolvedSrc;
      if (normalizedSrc !== resolvedSrc) {
        el.setAttribute("src", normalizedSrc);
      }

      const mappedSrc = toTauriLocalImageSrc(normalizedSrc);
      if (mappedSrc) {
        el.setAttribute('src', mappedSrc);
        (el as HTMLElement).style.maxWidth = '100%';
        (el as HTMLElement).style.height = 'auto';
      } else if (/^https?:\/\//i.test(normalizedSrc)) {
        // Some hosts reject unknown referrers; no-referrer is more broadly accepted.
        el.setAttribute("referrerpolicy", "no-referrer");
      }
    }
  });

  const bodyClone = doc.body.cloneNode(true) as HTMLElement;
  bodyClone.querySelectorAll("style, script, meta, link, xml").forEach(el => el.remove());
  
  const textContent = (bodyClone.textContent || "").trim();
  const TRUNCATION_MARKER = "... [HTML Truncated]";
  
  // If the ONLY content is the truncation marker, treat it as not renderable
  // so we fallback to the clean plain text preview.
  const hasRenderableText = textContent.length > 0 && textContent !== TRUNCATION_MARKER;
  const hasRenderableElement = !!bodyClone.querySelector("p, div, span, img, table, ul, ol, li, h1, h2, h3, h4, h5, h6, blockquote, pre");

  return {
    html: DOMPurify.sanitize(doc.body.innerHTML, PURIFY_CONFIG),
    hasRenderable: hasRenderableText || hasRenderableElement
  };
};

const blockLinkNavigation = (event: Event) => {
  // Links in copied HTML must never navigate the app window.
  if ((event.target as Element | null)?.closest?.("a")) {
    event.preventDefault();
  }
};

// Copied <style> blocks and :host rules stay inside the shadow root; the caller's
// container clips positioned content (see `contain: paint` below).
export const renderRichContent = (
  host: HTMLElement,
  htmlContent: string,
  options: { preview?: boolean; fallbackText?: string } = {}
) => {
  const { html: cleanHTML, hasRenderable } = sanitizeHTML(htmlContent, options.preview);
  let root = host.shadowRoot;
  if (!root) {
    root = host.attachShadow({ mode: "open" });
    root.addEventListener("click", blockLinkNavigation);
    root.addEventListener("auxclick", blockLinkNavigation);
  }
  const styleEl = document.createElement("style");
  styleEl.textContent = contentStyles;
  const body = document.createElement("div");
  body.className = "rich-text-body";
  if (host.closest(".compact-preview-root")) {
    body.classList.add("in-compact-preview");
  }
  if (!hasRenderable && options.fallbackText) {
    body.textContent = options.fallbackText;
  } else {
    body.innerHTML = cleanHTML;
  }
  root.replaceChildren(styleEl, body);
};

type HtmlContentProps = {
  htmlContent: string;
  fallbackText?: string;
  className?: string;
  style?: React.CSSProperties;
  preview?: boolean;
};

const HtmlContent = ({ htmlContent, fallbackText, className, style, preview }: HtmlContentProps) => {
  const contentRef = useRef<HTMLDivElement | null>(null);
  const hostRef = useRef<HTMLDivElement | null>(null);
  const processedRef = useRef<{ htmlContent: string; preview?: boolean; fallbackText?: string } | null>(null);
  const [isVisible, setIsVisible] = useState(!!preview);
  const previewPlaceholderMinHeight = (() => {
    const maxH = style?.maxHeight;
    if (typeof maxH === "number" && Number.isFinite(maxH)) return `${maxH}px`;
    if (typeof maxH === "string" && maxH.trim()) return maxH;
    return "40px";
  })();

  useEffect(() => {
    if (preview) return;
    if (!contentRef.current) return;

    const observer = new IntersectionObserver(
      ([entry]) => {
        if (entry.isIntersecting) {
          setIsVisible(true);
          observer.disconnect();
        }
      },
      { rootMargin: "200px" } // Load slightly before coming into view
    );

    observer.observe(contentRef.current);
    return () => observer.disconnect();
  }, [preview]);

  useLayoutEffect(() => {
    if (!isVisible || !hostRef.current) return;
    const prev = processedRef.current;
    if (prev && prev.htmlContent === htmlContent && prev.preview === preview && prev.fallbackText === fallbackText) {
      return;
    }
    processedRef.current = { htmlContent, preview, fallbackText };
    renderRichContent(hostRef.current, htmlContent, { preview, fallbackText });
  }, [htmlContent, fallbackText, isVisible, preview]);

  return (
    <div
      ref={contentRef}
      className={className}
      style={{
        minHeight: isVisible ? undefined : (preview ? previewPlaceholderMinHeight : '100px'),
        ...style,
        // Keeps fixed/absolute content from the clipboard inside this box.
        contain: "paint"
      }}
    >
      <div ref={hostRef} />
    </div>
  );
};

export default HtmlContent;
