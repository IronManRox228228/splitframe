/** Platform-appropriate wording for shortcuts and file-manager actions. */

/** "⌘D" on macOS, "Ctrl+D" elsewhere. */
export function shortcutLabel(key: string, platform: string | undefined): string {
  return platform === 'darwin' ? `⌘${key}` : `Ctrl+${key}`;
}

export function deleteKeyLabel(platform: string | undefined): string {
  return platform === 'darwin' ? '⌫' : 'Del';
}

export function revealLabel(platform: string | undefined): string {
  if (platform === 'darwin') return 'Show in Finder';
  if (platform === 'win32') return 'Show in Explorer';
  return 'Show in folder';
}
