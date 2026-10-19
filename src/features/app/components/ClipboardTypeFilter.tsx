import { motion, useReducedMotion } from "framer-motion";

const TYPES = ["text", "image", "file", "url", "code", "video", "rich_text"];

export default function ClipboardTypeFilter({ value, onChange, label }: {
  value: string | null;
  onChange: (value: string | null) => void;
  label: (type: string) => string;
}) {
  const reduceMotion = useReducedMotion();
  return <div className="type-filter-scroll hide-scrollbar" onWheel={event => {
    if (event.deltaY !== 0) event.currentTarget.scrollLeft += event.deltaY;
  }}>
    <div className="type-filter-track">
      {TYPES.map(type => <button
        key={type}
        type="button"
        className={`btn-icon type-filter-btn ${value === type ? "active" : ""}`}
        aria-pressed={value === type}
        onClick={() => onChange(value === type ? null : type)}
        title={label(type)}
      >
        {/* One selection plate slides between filters instead of two fills cross-fading. */}
        {value === type && <motion.span layoutId="type-filter-selection" className="type-filter-selection"
          transition={reduceMotion ? { duration: 0 } : { type: "spring", stiffness: 520, damping: 40, mass: .8 }} />}
        <span className="type-filter-label">{label(type)}</span>
      </button>)}
    </div>
  </div>;
}
