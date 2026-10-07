/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/renderer/**/*.{ts,tsx,html}'],
  theme: {
    extend: {
      colors: {
        // SplitFrame "Graphite": near-black ground, hairline borders, one accent for live state
        surface: {
          950: '#0a0a0b', // app ground
          900: '#0d0d0f', // panels
          850: '#141417', // raised: inputs, cards
          800: '#1c1c20', // hover / selected
          700: '#26262b', // strong border, pressed
          600: '#33333a'
        },
        accent: {
          DEFAULT: '#8b93ff',
          hover: '#a3a9ff',
          dim: '#2a2c52'
        },
        fg: {
          DEFAULT: '#ededef',
          2: '#c9c9d1',
          muted: '#9c9ca5',
          faint: '#85858e'
        },
        primary: {
          DEFAULT: '#f2f2f4',
          hover: '#ffffff'
        },
        danger: '#ff8a80',
        success: '#5ee0a0',
        line: '#1c1c20'
      },
      fontFamily: {
        sans: ['Geist', 'system-ui', 'sans-serif'],
        mono: ['"Geist Mono"', 'ui-monospace', 'monospace']
      },
      borderRadius: {
        DEFAULT: '8px',
        lg: '12px',
        xl: '14px',
        '2xl': '20px'
      },
      fontSize: {
        xs: ['12px', '16px'],
        sm: ['13px', '18px']
      }
    }
  },
  plugins: []
};
