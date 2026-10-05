import { cloneElement, type CSSProperties, type InputHTMLAttributes, type ReactElement } from "react";
import GlassLens from "../../../shared/components/GlassLens";

type FieldProps = InputHTMLAttributes<HTMLInputElement>;

// Inputs and selects cannot contain decorative children. Keep their real DOM
// control and event handlers inside one measured optical surface instead.
export default function SettingsGlassField({ theme, children }: { theme: string; children: ReactElement<FieldProps> }) {
    if (theme !== "liquid-glass") return children;
    const { style, type, value, min = 0, max = 100 } = children.props;
    if (type === "range") {
        const progress = Math.min(100, Math.max(0, (Number(value) - Number(min)) / (Number(max) - Number(min)) * 100));
        return <span className="settings-glass-range glass-slider-control">
            {cloneElement(children, { className: `${children.props.className || ""} settings-glass-range-input glass-material-slider` })}
            <span className="settings-glass-range-thumb glass-slider-thumb" aria-hidden="true" style={{ left: `calc(${progress}% - ${progress * .3}px)` }}>
                <GlassLens radius={11} />
            </span>
        </span>;
    }
    const layout: CSSProperties = { width: style?.width || style?.minWidth || "100%", minWidth: style?.minWidth, maxWidth: style?.maxWidth, flex: style?.flex,
        margin: style?.margin, marginLeft: style?.marginLeft, marginRight: style?.marginRight, marginTop: style?.marginTop, marginBottom: style?.marginBottom };
    return <span className="settings-glass-field" style={layout}>
        <GlassLens radius={12} />
        {cloneElement(children, { style: { ...style, width: "100%", minWidth: 0, maxWidth: "100%", margin: 0, marginLeft: 0, marginRight: 0, marginTop: 0, marginBottom: 0 } })}
    </span>;
}
