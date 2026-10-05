// Native lenses occupy their actual .glass-lens stacking contexts. Resolve
// those contexts atomically, including ancestors which are not glass surfaces.
// https://www.w3.org/TR/CSS22/zindex.html
export function glassPaintOrder(elements: HTMLElement[]): HTMLElement[] {
  const styles = new Map<HTMLElement, CSSStyleDeclaration>();
  const paths = new Map<HTMLElement, HTMLElement[]>();
  const style = (element: HTMLElement) => {
    let value = styles.get(element);
    if (!value) { value = getComputedStyle(element); styles.set(element, value); }
    return value;
  };
  const flexOrGrid = (element: HTMLElement) => /(?:^|inline-)(?:flex|grid)$/.test(style(element).display);
  const positionedItem = (element: HTMLElement) => {
    const value = style(element);
    return value.position !== "static" || !!(element.parentElement && flexOrGrid(element.parentElement));
  };
  const level = (element: HTMLElement) => positionedItem(element) && style(element).zIndex !== "auto"
    ? Number(style(element).zIndex) || 0 : 0;
  const createsContext = (element: HTMLElement) => {
    const value = style(element);
    return element === document.documentElement || /^(fixed|sticky)$/.test(value.position)
      || (value.zIndex !== "auto" && positionedItem(element)) || Number(value.opacity) < 1
      || value.isolation === "isolate" || value.mixBlendMode !== "normal"
      || /\b(layout|paint|strict|content)\b/.test(value.contain)
      || /^(size|inline-size)$/.test(value.containerType)
      || ["transform", "scale", "rotate", "translate", "perspective", "filter", "backdrop-filter", "clip-path", "mask-image", "-webkit-mask-image"]
        .some(property => { const current = value.getPropertyValue(property); return current !== "" && current !== "none"; })
      || value.willChange.split(",").some(property => /^(opacity|transform|scale|rotate|translate|perspective|filter|backdrop-filter|clip-path|mask|mask-image|mix-blend-mode|isolation)$/.test(property.trim()));
  };
  const contextPath = (element: HTMLElement): HTMLElement[] => {
    let path = paths.get(element);
    if (!path) {
      const parent = element.parentElement ? contextPath(element.parentElement) : [];
      path = createsContext(element) ? [...parent, element] : parent;
      paths.set(element, path);
    }
    return path;
  };
  const treeOrder = (first: HTMLElement, second: HTMLElement) => {
    if (first === second) return 0;
    const firstAncestors: HTMLElement[] = [];
    for (let node: HTMLElement | null = first; node; node = node.parentElement) firstAncestors.push(node);
    let secondBranch = second;
    while (secondBranch.parentElement && !firstAncestors.includes(secondBranch.parentElement)) secondBranch = secondBranch.parentElement;
    const common = secondBranch.parentElement;
    if (common && flexOrGrid(common)) {
      const firstBranch = firstAncestors[firstAncestors.indexOf(common) - 1];
      if (firstBranch) {
        const order = (node: HTMLElement) => /^(absolute|fixed)$/.test(style(node).position) ? 0 : Number(style(node).order) || 0;
        const difference = order(firstBranch) - order(secondBranch);
        if (difference) return difference;
      }
    }
    return first.compareDocumentPosition(second) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1;
  };
  return [...elements].sort((first, second) => {
    const a = contextPath(first), b = contextPath(second);
    let index = 0;
    while (index < a.length && index < b.length && a[index] === b[index]) ++index;
    if (index === a.length || index === b.length) {
      // A context's own surface precedes its descendant contexts. GlassLens
      // itself is positioned with z-index, so eligible lenses have a leaf here.
      return a.length === b.length ? treeOrder(first, second) : a.length - b.length;
    }
    return level(a[index]) - level(b[index]) || treeOrder(a[index], b[index]);
  });
}
