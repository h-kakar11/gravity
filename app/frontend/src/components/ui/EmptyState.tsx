// A deliberate empty state (spec section 12): every "nothing here" screen explains what's
// missing and, where there's a sensible next step, offers exactly one action for it. Never
// bare whitespace.

import type { ReactNode } from "react";

interface EmptyStateProps {
  icon: ReactNode;
  title: string;
  description?: string;
  action?: ReactNode;
}

export function EmptyState({ icon, title, description, action }: EmptyStateProps) {
  return (
    <div className="gv-empty" role="status">
      <div className="gv-empty__icon">{icon}</div>
      <div className="gv-empty__title">{title}</div>
      {description ? <div className="gv-empty__desc">{description}</div> : null}
      {action ? <div className="gv-empty__action">{action}</div> : null}
    </div>
  );
}
