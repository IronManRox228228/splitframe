/**
 * The routing table for the AI settings page (16-AGENT-crew Part A item 1): one row per
 * SplitFrame call-site task, showing the class the task lands in, the request route (chat or
 * 3d) and the meterless-accept header value.
 *
 * Boundary: these rows are a static mirror of the native router (`sf::meterless_router::
 * routingTable()` in native/src/agent/meterless_router.cpp), which is the source of truth —
 * its assertions live in native/tests/tst_meterless_router.cpp. This copy exists only to
 * render the read-only table when the desktop UI runs without a native backend attached;
 * the rows here describe behavior, they never change it.
 */

export interface RoutingRow {
  task: string;
  className: string;
  route: 'chat' | '3d';
  /** meterless-accept header value; empty = no accept header */
  accept: string;
}

export const ROUTING_TABLE: RoutingRow[] = [
  { task: 'commentary', className: 'text', route: 'chat', accept: '' },
  { task: 'planning', className: 'text', route: 'chat', accept: '' },
  { task: 'tool_call', className: 'text', route: 'chat', accept: '' },
  { task: 'storyboard', className: 'text', route: 'chat', accept: '' },
  { task: 'generation_prompt', className: 'text', route: 'chat', accept: '' },
  { task: 'chat', className: 'text', route: 'chat', accept: '' },
  { task: 'summary', className: 'text', route: 'chat', accept: '' },
  { task: 'code', className: 'code', route: 'chat', accept: '' },
  { task: 'peer_review', className: 'judge', route: 'chat', accept: '' },
  { task: 'grade', className: 'judge', route: 'chat', accept: '' },
  { task: 'screenshot', className: 'vision', route: 'chat', accept: '' },
  { task: 'frame_grab', className: 'vision', route: 'chat', accept: '' },
  { task: 'document', className: 'perception', route: 'chat', accept: '' },
  { task: 'pdf', className: 'perception', route: 'chat', accept: '' },
  { task: 'transcribe', className: 'transcribe', route: 'chat', accept: '' },
  { task: 'voice', className: 'speech', route: 'chat', accept: 'audio' },
  { task: 'speech', className: 'speech', route: 'chat', accept: 'audio' },
  { task: 'theme', className: 'music', route: 'chat', accept: 'audio' },
  { task: 'music', className: 'music', route: 'chat', accept: 'audio' },
  { task: 'embed', className: 'embed', route: 'chat', accept: '' },
  { task: 'search', className: 'embed', route: 'chat', accept: '' },
  { task: 'still', className: 'image', route: 'chat', accept: 'image' },
  { task: 'thumbnail', className: 'image', route: 'chat', accept: 'image' },
  { task: 'video', className: 'video', route: 'chat', accept: 'video' },
  { task: 'preview_render', className: 'video', route: 'chat', accept: 'video' },
  { task: 'model_3d', className: 'mesh', route: '3d', accept: 'mesh' },
  { task: 'mesh', className: 'mesh', route: '3d', accept: 'mesh' },
  { task: 'repaint', className: 'texture', route: '3d', accept: 'mesh' },
  { task: 'texture', className: 'texture', route: '3d', accept: 'mesh' },
  { task: 'paint', className: 'texture', route: '3d', accept: 'mesh' },
];
