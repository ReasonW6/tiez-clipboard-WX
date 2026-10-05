import GlassLens from "../../../shared/components/GlassLens";
import "../../../styles/themes/liquid-glass-filter.css";

const TYPES = ["text", "image", "file", "url", "code", "video", "rich_text"];

export default function ClipboardTypeFilter({ theme, value, onChange, label }: {
  theme: string;
  value: string | null;
  onChange: (value: string | null) => void;
  label: (type: string) => string;
}) {
  const liquid = theme === "liquid-glass";

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
      >{liquid && <GlassLens radius={16} />}<span className="type-filter-label">{label(type)}</span></button>)}
    </div>
  </div>;
}
