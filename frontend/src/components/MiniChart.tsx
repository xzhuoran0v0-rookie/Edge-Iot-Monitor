import type { SeriesPoint } from "../types";

interface Props {
  data: SeriesPoint[];
  color: string;
  height?: number;
}

export default function MiniChart({ data, color, height = 120 }: Props) {
  if (data.length < 2) {
    return <div className="chart-empty">Waiting for more data points…</div>;
  }

  const values = data.map((d) => d.value);
  const min = Math.min(...values);
  const max = Math.max(...values);
  const range = max - min || 1;

  const W = 560;
  const H = height;
  const padY = 16;
  const padX = 40;
  const chartW = W - padX;
  const chartH = H - padY * 2;

  const points = values.map((v, i) => {
    const x = padX + (i / (values.length - 1)) * chartW;
    const y = padY + chartH - ((v - min) / range) * chartH;
    return { x, y, v };
  });

  const line = points.map((p, i) => `${i === 0 ? "M" : "L"}${p.x},${p.y}`).join(" ");
  const area = `${line} L${points[points.length - 1].x},${H - padY} L${padX},${H - padY} Z`;

  const gridLines = 4;
  const gridValues = Array.from({ length: gridLines }, (_, i) =>
    +(min + (range * i) / (gridLines - 1)).toFixed(1)
  );

  return (
    <div className="chart-container">
      <svg viewBox={`0 0 ${W} ${H}`} xmlns="http://www.w3.org/2000/svg">
        {gridValues.map((gv, i) => {
          const y = padY + chartH - ((gv - min) / range) * chartH;
          return (
            <g key={i}>
              <line
                x1={padX}
                y1={y}
                x2={W}
                y2={y}
                stroke="#e5e5e5"
                strokeWidth="0.5"
              />
              <text
                x={padX - 6}
                y={y + 3}
                textAnchor="end"
                fontSize="9"
                fill="#a3a3a3"
                fontFamily="var(--mono)"
              >
                {gv}
              </text>
            </g>
          );
        })}
        <path d={area} fill={color} opacity="0.08" />
        <path d={line} fill="none" stroke={color} strokeWidth="1.5" strokeLinejoin="round" />
        {points.length > 0 && (
          <circle
            cx={points[points.length - 1].x}
            cy={points[points.length - 1].y}
            r="3"
            fill={color}
          />
        )}
      </svg>
    </div>
  );
}
