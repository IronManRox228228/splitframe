import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './App.tsx';
import { useEditor } from './store.ts';
import { MOTION_RUNTIME_SOURCE, MOTION_EVAL_SOURCE } from '@cutboard/renderer';
import './styles.css';

void React;
// debug/smoke affordance: drive the store from the console or the smoke driver
(window as unknown as Record<string, unknown>)['__cutboardStore'] = useEditor;
// motion-graphics sandbox runtime source (provisioned into sandboxed iframes)
(window as unknown as Record<string, unknown>)['__cutboardMotionRuntime'] = MOTION_RUNTIME_SOURCE + MOTION_EVAL_SOURCE;
const root = createRoot(document.getElementById('root')!);
root.render(<App />);
