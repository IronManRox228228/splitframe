/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/renderer/**/*.{ts,tsx,html}'],
  theme: {
    extend: {
      colors: {
        // SplitFrame "clean glass": flat near-black ground, glass slabs, one mint accent for live state
        surface: {
          950: '#050606', // app ground
          900: '#0A0C0B', // solid panel fallback
          850: '#0D0F0E', // inputs, raised (sits on glass)
          800: '#1B1F1D', // hover / selected
          700: '#343A37', // strong border, pressed
          600: '#4A514E'
        },
        accent: {
          DEFAULT: '#5FB7A1',
          hover: '#7BC9B5',
          dim: '#215C4F',
          deep: '#215C4F'
        },
        fg: {
          DEFAULT: '#ECEEEC',
          2: '#C4CAC7',
          muted: '#8E9793',
          faint: '#78837E' // 4.8:1 on #050606, 4.5+:1 on glass
        },
        primary: {
          DEFAULT: '#ECEEEC',
          hover: '#FFFFFF'
        },
        danger: '#F28B82',
        success: '#86D39B',
        line: 'rgba(236,238,236,0.10)',
        'line-strong': 'rgba(236,238,236,0.22)'
      },
      fontFamily: {
        sans: ['Geist', 'system-ui', 'sans-serif'],
        mono: ['"Geist Mono"', 'ui-monospace', 'monospace']
      },
      borderRadius: {
        DEFAULT: '8px',
        lg: '12px',
        xl: '14px',
        '2xl': '22px'
      },
      fontSize: {
        xs: ['12px', '16px'],
        sm: ['13px', '18px']
      }
    }
  },
  plugins: []
};
