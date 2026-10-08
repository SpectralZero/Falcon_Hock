import { useId } from "react";
import { BRAND } from "../brand";

type MarkProps = {
  size?: number;
  glow?: boolean;
  className?: string;
};

/**
 * The Hexforge mark: an "H" monogram built from circuit traces, set inside a
 * hexagon, filled with the violet->cyan brand gradient. Vector + themeable.
 */
export function LogoMark({ size = 28, glow = false, className }: MarkProps) {
  const gid = useId().replace(/:/g, "");
  const grad = `hx-grad-${gid}`;
  const soft = `hx-soft-${gid}`;
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 48 48"
      fill="none"
      className={className}
      role="img"
      aria-label={`${BRAND.name} logo`}
      style={glow ? { filter: `drop-shadow(0 0 10px rgba(124,92,255,0.45))` } : undefined}
    >
      <defs>
        <linearGradient id={grad} x1="4" y1="4" x2="44" y2="44" gradientUnits="userSpaceOnUse">
          <stop offset="0" stopColor="#7C5CFF" />
          <stop offset="1" stopColor="#35E0D0" />
        </linearGradient>
        <linearGradient id={soft} x1="24" y1="6" x2="24" y2="42" gradientUnits="userSpaceOnUse">
          <stop offset="0" stopColor="#7C5CFF" stopOpacity="0.22" />
          <stop offset="1" stopColor="#35E0D0" stopOpacity="0.05" />
        </linearGradient>
      </defs>

      {/* hex plate fill */}
      <path
        d="M24 4 L41.3 14 L41.3 34 L24 44 L6.7 34 L6.7 14 Z"
        fill={`url(#${soft})`}
      />
      {/* hex border */}
      <path
        d="M24 4 L41.3 14 L41.3 34 L24 44 L6.7 34 L6.7 14 Z"
        stroke={`url(#${grad})`}
        strokeWidth="2.2"
        strokeLinejoin="round"
      />

      {/* circuit trace stubs */}
      <path d="M18 16 L18 10" stroke="#35E0D0" strokeWidth="1.4" strokeLinecap="round" opacity="0.85" />
      <circle cx="18" cy="9.4" r="1.5" fill="#35E0D0" />
      <path d="M30 32 L30 38" stroke="#7C5CFF" strokeWidth="1.4" strokeLinecap="round" opacity="0.85" />
      <circle cx="30" cy="38.6" r="1.5" fill="#7C5CFF" />

      {/* H monogram */}
      <path
        d="M18 16 L18 32 M30 16 L30 32 M18 24 L30 24"
        stroke={`url(#${grad})`}
        strokeWidth="3.2"
        strokeLinecap="round"
        strokeLinejoin="round"
      />
      {/* node */}
      <circle cx="24" cy="24" r="3.1" fill="#0A0C12" stroke={`url(#${grad})`} strokeWidth="2" />
    </svg>
  );
}

type LockupProps = {
  size?: number;
  showSub?: boolean;
  compact?: boolean;
};

/** Mark + HEXFORGE / STUDIO wordmark lockup. */
export function BrandLockup({ size = 30, showSub = true, compact = false }: LockupProps) {
  return (
    <div className="brand-lockup">
      <LogoMark size={size} glow />
      <div className="brand-words" style={compact ? { gap: 0 } : undefined}>
        <span className="brand-word" style={{ fontSize: size * 0.52 }}>
          {BRAND.wordmark}
        </span>
        {showSub && <span className="brand-sub">{BRAND.sub}</span>}
      </div>
    </div>
  );
}
