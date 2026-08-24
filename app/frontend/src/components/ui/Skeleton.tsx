// Loading placeholders (spec section 14). Used where a screen has real, known structure
// while its data is in flight -- so the app never looks frozen and never overuses a bare
// spinner for content that has an obvious shape.

export function Skeleton({ width, height = "1em", radius = 6 }: { width: string | number; height?: string | number; radius?: number }) {
  return (
    <span
      className="gv-skeleton"
      style={{ width, height, borderRadius: radius, display: "inline-block" }}
      aria-hidden="true"
    />
  );
}

export function SkeletonRow() {
  return (
    <div className="gv-row gv-skeleton-row" aria-hidden="true">
      <div style={{ flex: 1, display: "flex", flexDirection: "column", gap: 8 }}>
        <Skeleton width={90} height={16} radius={999} />
        <Skeleton width="55%" height={16} />
        <Skeleton width="30%" height={12} />
        <Skeleton width="100%" height={6} radius={999} />
      </div>
    </div>
  );
}
