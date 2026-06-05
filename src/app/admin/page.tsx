'use client'

import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useRouter } from 'next/navigation'
import { useEffect, useState } from 'react'
import { api } from '@/trpc/react'
import { BrandHeader } from '@/components/brand-header'

const ADMIN_NPUB = 'npub13hyx3qsqk3r7ctjqrr49uskut4yqjsxt8uvu4rekr55p08wyhf0qq90nt7'

export default function AdminPage() {
  const { user, isConnected } = useNostrAuth()
  const router = useRouter()
  const [timeRange, setTimeRange] = useState<30 | 90>(30)

  useEffect(() => {
    if (!isConnected) {
      router.push('/')
      return
    }

    if (user?.npub !== ADMIN_NPUB) {
      router.push('/')
    }
  }, [user, isConnected, router])

  const { data: stats, isLoading: statsLoading } = api.admin.getStats.useQuery(
    undefined,
    { enabled: isConnected && user?.npub === ADMIN_NPUB }
  )

  const { data: userGrowth, isLoading: growthLoading } = api.admin.getUserGrowth.useQuery(
    { days: timeRange },
    { enabled: isConnected && user?.npub === ADMIN_NPUB }
  )

  const { data: activity, isLoading: activityLoading } = api.admin.getActivityHistory.useQuery(
    { days: timeRange },
    { enabled: isConnected && user?.npub === ADMIN_NPUB }
  )

  if (!isConnected || user?.npub !== ADMIN_NPUB) {
    return null
  }

  const isLoading = statsLoading || growthLoading || activityLoading

  return (
    <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      <BrandHeader cta={{ href: '/reader', label: 'Go to Reader' }} />

      <div className="mx-auto w-full max-w-7xl px-4 py-12 sm:px-6 lg:px-8">
        <div className="mb-8">
          <h1 className="text-3xl font-extrabold tracking-tight md:text-4xl">
            <span className="bg-gradient-to-br from-[#27ae60] via-[#2ecc71] to-[#58d68d] bg-clip-text text-transparent">
              Readstr
            </span>{' '}
            Admin Dashboard
          </h1>
          <p className="mt-2 text-base text-[#B3B3B3]">
            Welcome, Administrator
          </p>
        </div>

        {/* Time Range Selector */}
        <div className="mb-6 flex gap-2">
          <button
            onClick={() => setTimeRange(30)}
            className={`rounded-xl px-4 py-2 text-sm font-semibold transition-all duration-300 ${
              timeRange === 30
                ? 'bg-gradient-to-br from-[#27ae60] to-[#229954] text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)]'
                : 'border border-[#27ae60]/25 bg-white/10 text-[#B3B3B3] hover:border-[#27ae60]/50 hover:bg-[#27ae60]/10 hover:text-white'
            }`}
          >
            Last 30 Days
          </button>
          <button
            onClick={() => setTimeRange(90)}
            className={`rounded-xl px-4 py-2 text-sm font-semibold transition-all duration-300 ${
              timeRange === 90
                ? 'bg-gradient-to-br from-[#27ae60] to-[#229954] text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)]'
                : 'border border-[#27ae60]/25 bg-white/10 text-[#B3B3B3] hover:border-[#27ae60]/50 hover:bg-[#27ae60]/10 hover:text-white'
            }`}
          >
            Last 90 Days
          </button>
        </div>

        {isLoading ? (
          <div className="py-12 text-center">
            <div className="inline-block h-12 w-12 animate-spin rounded-full border-b-2 border-[#27ae60]"></div>
            <p className="mt-4 text-[#B3B3B3]">Loading statistics...</p>
          </div>
        ) : (
          <>
            {/* Stats Overview */}
            <div className="mb-8 grid grid-cols-1 gap-6 md:grid-cols-2 lg:grid-cols-4">
              <div className="rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl">
                <h2 className="mb-1 text-sm font-medium text-[#B3B3B3]">
                  Total Users
                </h2>
                <p className="text-3xl font-bold text-white">
                  {stats?.users.total || 0}
                </p>
                <p className="mt-1 text-xs text-[#B3B3B3]/70">
                  {stats?.users.active7d || 0} active (7d) • {stats?.users.active30d || 0} active (30d)
                </p>
              </div>

              <div className="rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl">
                <h2 className="mb-1 text-sm font-medium text-[#B3B3B3]">
                  Total Feeds
                </h2>
                <p className="text-3xl font-bold text-white">
                  {stats?.feeds.total || 0}
                </p>
                <p className="mt-1 text-xs text-[#B3B3B3]/70">
                  {stats?.feeds.active || 0} active • {stats?.feeds.guideFeeds || 0} in guide
                </p>
              </div>

              <div className="rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl">
                <h2 className="mb-1 text-sm font-medium text-[#B3B3B3]">
                  Feed Items
                </h2>
                <p className="text-3xl font-bold text-white">
                  {stats?.items.total.toLocaleString() || 0}
                </p>
                <p className="mt-1 text-xs text-[#B3B3B3]/70">
                  {stats?.items.read30d || 0} read (30d)
                </p>
              </div>

              <div className="rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl">
                <h2 className="mb-1 text-sm font-medium text-[#B3B3B3]">
                  Subscriptions
                </h2>
                <p className="text-3xl font-bold text-white">
                  {stats?.subscriptions.total || 0}
                </p>
                <p className="mt-1 text-xs text-[#B3B3B3]/70">
                  {stats?.subscriptions.paid || 0} paid • {stats?.subscriptions.trial || 0} trial
                </p>
              </div>
            </div>

            {/* User Growth Chart */}
            <div className="mb-8 rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl">
              <h2 className="mb-4 text-xl font-semibold text-white">
                User Growth (Cumulative)
              </h2>
              <div className="flex h-64 items-end justify-between gap-1">
                {userGrowth?.map((day, idx) => {
                  const maxCount = Math.max(...(userGrowth?.map(d => d.count) || [1]))
                  const height = maxCount > 0 ? (day.count / maxCount) * 100 : 0
                  const isRecent = idx >= userGrowth.length - 7

                  return (
                    <div key={day.date} className="group relative flex flex-1 flex-col items-center">
                      <div
                        className={`w-full rounded-t transition-all ${
                          isRecent ? 'bg-[#2ecc71]' : 'bg-[#27ae60]/60'
                        } hover:bg-[#58d68d]`}
                        style={{ height: `${height}%`, minHeight: day.count > 0 ? '2px' : '0' }}
                      />
                      <div className="absolute bottom-full z-10 mb-2 hidden whitespace-nowrap rounded bg-[#0d1117] px-2 py-1 text-xs text-white group-hover:block">
                        {new Date(day.date).toLocaleDateString()}: {day.count} users
                      </div>
                    </div>
                  )
                })}
              </div>
              <div className="mt-2 flex justify-between text-xs text-[#B3B3B3]">
                <span>{userGrowth?.[0]?.date ? new Date(userGrowth[0].date).toLocaleDateString() : ''}</span>
                <span>{userGrowth?.[userGrowth.length - 1]?.date ? new Date(userGrowth[userGrowth.length - 1].date).toLocaleDateString() : ''}</span>
              </div>
            </div>

            {/* Activity Chart */}
            <div className="mb-8 rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl">
              <h2 className="mb-4 text-xl font-semibold text-white">
                Daily Activity (Items Read)
              </h2>
              <div className="flex h-64 items-end justify-between gap-1">
                {activity?.map((day, idx) => {
                  const maxReads = Math.max(...(activity?.map(d => d.reads) || [1]))
                  const height = maxReads > 0 ? (day.reads / maxReads) * 100 : 0
                  const isRecent = idx >= activity.length - 7

                  return (
                    <div key={day.date} className="group relative flex flex-1 flex-col items-center">
                      <div
                        className={`w-full rounded-t transition-all ${
                          isRecent ? 'bg-[#58d68d]' : 'bg-[#27ae60]/50'
                        } hover:bg-[#2ecc71]`}
                        style={{ height: `${height}%`, minHeight: day.reads > 0 ? '2px' : '0' }}
                      />
                      <div className="absolute bottom-full z-10 mb-2 hidden whitespace-nowrap rounded bg-[#0d1117] px-2 py-1 text-xs text-white group-hover:block">
                        {new Date(day.date).toLocaleDateString()}: {day.reads} reads
                      </div>
                    </div>
                  )
                })}
              </div>
              <div className="mt-2 flex justify-between text-xs text-[#B3B3B3]">
                <span>{activity?.[0]?.date ? new Date(activity[0].date).toLocaleDateString() : ''}</span>
                <span>{activity?.[activity.length - 1]?.date ? new Date(activity[activity.length - 1].date).toLocaleDateString() : ''}</span>
              </div>
            </div>

            {/* Current User Info */}
            <div className="rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl">
              <h2 className="mb-4 text-xl font-semibold text-white">
                Current Session
              </h2>
              <div className="space-y-2 text-sm">
                <p className="text-[#B3B3B3]">
                  <span className="font-semibold text-white">npub:</span> {user?.npub}
                </p>
                <p className="text-[#B3B3B3]">
                  <span className="font-semibold text-white">pubkey:</span> {user?.pubkey}
                </p>
              </div>
            </div>
          </>
        )}
      </div>
    </main>
  )
}
