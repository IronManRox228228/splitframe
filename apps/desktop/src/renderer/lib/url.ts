/** cbmedia:// URL builder for local media (registered in the main process). */
export function mediaUrl(absolutePath: string): string {
  return `cbmedia://media/${encodeURIComponent(absolutePath)}`;
}
