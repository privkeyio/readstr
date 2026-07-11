'use client'

import type { SavedView } from '@/lib/saved-views'

interface SavedViewsBarProps {
  views: SavedView[]
  activeViewId: string | null
  onSelectCurrent: () => void
  onSelectView: (view: SavedView) => void
  onSaveCurrent: () => void
}

export function SavedViewsBar({
  views,
  activeViewId,
  onSelectCurrent,
  onSelectView,
  onSaveCurrent,
}: SavedViewsBarProps) {
  const chipClass = (active: boolean) =>
    `flex-shrink-0 flex items-center gap-1.5 px-3 py-1.5 rounded-full text-sm font-medium border transition-all duration-150 ${
      active
        ? 'bg-theme-accent-light border-theme-accent text-theme-accent shadow-theme-sm'
        : 'bg-theme-tertiary border-transparent text-theme-secondary hover:text-theme-primary hover:bg-theme-hover'
    }`

  return (
    <div className="mt-3 flex items-center gap-2 overflow-x-auto themed-scrollbar pb-1">
      <button onClick={onSelectCurrent} className={chipClass(activeViewId === null)}>
        Current
      </button>
      {views.map((view) => (
        <button
          key={view.id}
          onClick={() => onSelectView(view)}
          className={chipClass(activeViewId === view.id)}
          title={view.name}
        >
          {view.icon && <span>{view.icon}</span>}
          <span className="truncate max-w-[10rem]">{view.name}</span>
        </button>
      ))}
      <button
        onClick={onSaveCurrent}
        className="flex-shrink-0 w-8 h-8 flex items-center justify-center rounded-full bg-theme-tertiary text-theme-secondary hover:text-theme-primary hover:bg-theme-hover transition-colors"
        title="Save current as view"
      >
        +
      </button>
    </div>
  )
}
