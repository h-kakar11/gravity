// Button variants for the whole app (spec section 3): Primary, Secondary, Destructive,
// Ghost, and Disabled -- one component so every button in Gravity shares padding, radius,
// transition and focus behaviour instead of re-deriving them per page.

import { forwardRef, type ButtonHTMLAttributes } from "react";
import { SpinnerIcon } from "../icons";

export type ButtonVariant = "primary" | "secondary" | "destructive" | "ghost";
export type ButtonSize = "sm" | "md";

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  busy?: boolean;
  icon?: React.ReactNode;
}

export const Button = forwardRef<HTMLButtonElement, ButtonProps>(function Button(
  { variant = "secondary", size = "md", busy = false, icon, children, className, disabled, ...rest },
  ref,
) {
  const classes = ["gv-btn", `gv-btn--${variant}`, `gv-btn--${size}`, className]
    .filter(Boolean)
    .join(" ");
  return (
    <button ref={ref} className={classes} disabled={disabled || busy} {...rest}>
      {busy ? <SpinnerIcon size={size === "sm" ? 13 : 15} /> : icon}
      {children ? <span>{children}</span> : null}
    </button>
  );
});
