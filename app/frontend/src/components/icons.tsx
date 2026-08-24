// One coherent icon system (spec section 17): hand-authored 20x20 stroke icons in a single
// visual language, so nothing in the app mixes an emoji, a unicode arrow, and a third-party
// icon font. Every icon is decorative by default (aria-hidden) -- the accessible name for an
// icon button comes from its own aria-label/title, never from the glyph.

import type { ReactNode, SVGProps } from "react";

type IconProps = SVGProps<SVGSVGElement> & { size?: number };

function Icon({ size = 18, children, ...rest }: IconProps & { children: ReactNode }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 20 20"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.6}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
      {...rest}
    >
      {children}
    </svg>
  );
}

export const HomeIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M3 9.5 10 3l7 6.5" />
    <path d="M5 8.5V17h10V8.5" />
  </Icon>
);

export const DownloadIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M10 3v9" />
    <path d="m6.2 8.6 3.8 3.8 3.8-3.8" />
    <path d="M4 15.5v1a1.5 1.5 0 0 0 1.5 1.5h9a1.5 1.5 0 0 0 1.5-1.5v-1" />
  </Icon>
);

export const ConvertIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M4 7h9.5" />
    <path d="m11 4 2.8 3-2.8 3" />
    <path d="M16 13H6.5" />
    <path d="m9 10-2.8 3 2.8 3" />
  </Icon>
);

export const CompressIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M10 3v4.5M10 12.5V17" />
    <path d="m7 6 3 1.5L13 6" />
    <path d="m7 14 3-1.5L13 14" />
    <rect x="5.5" y="7.5" width="9" height="5" rx="1.2" />
  </Icon>
);

export const QueueIcon = (p: IconProps) => (
  <Icon {...p}>
    <rect x="3" y="4.5" width="14" height="3" rx="1" />
    <rect x="3" y="9.5" width="14" height="3" rx="1" opacity="0.7" />
    <rect x="3" y="14.5" width="9" height="2.6" rx="1" opacity="0.45" />
  </Icon>
);

export const SettingsIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="10" cy="10" r="2.6" />
    <path d="M10 3.3v1.6M10 15.1v1.6M16.7 10h-1.6M4.9 10H3.3M14.7 5.3l-1.1 1.1M6.4 13.6l-1.1 1.1M14.7 14.7l-1.1-1.1M6.4 6.4 5.3 5.3" />
  </Icon>
);

export const PlayIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M6 4.5v11l9-5.5-9-5.5Z" />
  </Icon>
);

export const PauseIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M6.5 4.5v11M13.5 4.5v11" />
  </Icon>
);

export const RetryIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M15.5 10a5.5 5.5 0 1 1-1.9-4.16" />
    <path d="M15.5 3.5v3.6h-3.6" />
  </Icon>
);

export const CancelIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="10" cy="10" r="6.5" />
    <path d="m7.5 7.5 5 5M12.5 7.5l-5 5" />
  </Icon>
);

export const TrashIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M4.5 6h11M8 6V4.6c0-.55.45-1 1-1h2c.55 0 1 .45 1 1V6M6.2 6l.6 9.3c.04.65.58 1.2 1.24 1.2h4.32c.66 0 1.2-.55 1.24-1.2L14.2 6" />
  </Icon>
);

export const ChevronUpIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m5.5 12 4.5-5 4.5 5" />
  </Icon>
);

export const ChevronDownIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m5.5 8 4.5 5 4.5-5" />
  </Icon>
);

export const ChevronToTopIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m5 10.5 5-5 5 5" />
    <path d="M5 15h10" />
  </Icon>
);

export const CheckCircleIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="10" cy="10" r="6.8" />
    <path d="m7 10.2 2.1 2.1L13.3 8" />
  </Icon>
);

export const AlertTriangleIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M10 3.5 17 16H3L10 3.5Z" />
    <path d="M10 8.3v3.4" />
    <circle cx="10" cy="14" r="0.15" fill="currentColor" stroke="none" />
  </Icon>
);

export const InfoIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="10" cy="10" r="6.8" />
    <path d="M10 9v4.2" />
    <circle cx="10" cy="6.8" r="0.15" fill="currentColor" stroke="none" />
  </Icon>
);

export const XIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="m5.5 5.5 9 9M14.5 5.5l-9 9" />
  </Icon>
);

export const FolderIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M3 6.2a1 1 0 0 1 1-1h3.4l1.4 1.6H16a1 1 0 0 1 1 1V15a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6.2Z" />
  </Icon>
);

export const LinkIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M8.3 11.7 11.7 8.3" />
    <path d="M9 6.3 10.4 5a2.6 2.6 0 1 1 3.6 3.6L12.7 10" />
    <path d="M11 13.7 9.6 15a2.6 2.6 0 1 1-3.6-3.6L7.3 10" />
  </Icon>
);

export const SearchIcon = (p: IconProps) => (
  <Icon {...p}>
    <circle cx="8.6" cy="8.6" r="4.8" />
    <path d="m15.5 15.5-3.3-3.3" />
  </Icon>
);

export const InboxIcon = (p: IconProps) => (
  <Icon {...p}>
    <path d="M4 11 6 4.6h8L16 11" />
    <path d="M4 11v4a1 1 0 0 0 1 1h10a1 1 0 0 0 1-1v-4h-3.4l-.9 1.6h-3.4L7.4 11H4Z" />
  </Icon>
);

export const SpinnerIcon = ({ size = 18, ...rest }: IconProps) => (
  <svg
    width={size}
    height={size}
    viewBox="0 0 20 20"
    fill="none"
    aria-hidden="true"
    focusable="false"
    className="gv-spin"
    {...rest}
  >
    <circle cx="10" cy="10" r="7.5" stroke="currentColor" strokeOpacity="0.22" strokeWidth="2.2" />
    <path
      d="M17.5 10a7.5 7.5 0 0 0-7.5-7.5"
      stroke="currentColor"
      strokeWidth="2.2"
      strokeLinecap="round"
    />
  </svg>
);
