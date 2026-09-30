/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/renderer/**/*.{ts,tsx,html}'],
  theme: {
    extend: {
      colors: {
        // Cutboard identity: near-black surfaces, warm amber accent, subtle borders
        surface: {
          950: '#0b0b0d',
          900: '#111114',
          850: '#16161a',
          800: '#1c1c22',
          700: '#26262e',
          600: '#33333d'
        },
        accent: {
          DEFAULT: '#fbbf24',
          hover: '#fcd34d',
          dim: '#92600a'
        },
        line: '#26262e'
      },
      borderRadius: {
        DEFAULT: '8px',
        lg: '12px'
      },
      fontSize: {
        xs: ['11px', '14px']
      }
    }
  },
  plugins: []
};
