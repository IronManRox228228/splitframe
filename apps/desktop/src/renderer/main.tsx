import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './App.tsx';
import { useEditor } from './store.ts';
import './styles.css';

void React;
// debug/smoke affordance: drive the store from the console or the smoke driver
(window as unknown as Record<string, unknown>)['__cutboardStore'] = useEditor;
const root = createRoot(document.getElementById('root')!);
root.render(<App />);
