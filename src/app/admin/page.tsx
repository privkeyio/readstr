'use client'

import { useNostrAuth } from '@/contexts/NostrAuthContext'
import { useRouter } from 'next/navigation'
import { useEffect, useState } from 'react'
import { api } from '@/trpc/react'

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
    <div className="min-h-screen bg-gray-50 dark:bg-gray-900">
      <div className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 py-8">
        <div className="mb-8">
          <h1 className="text-3xl font-bold text-gray-900 dark:text-white">
            Admin Dashboard
          </h1>
          <p className="mt-2 text-gray-600 dark:text-gray-400">
            Welcome, Administrator
          </p>
        </div>

        {/* Time Range Selector */}
        <div className="mb-6 flex gap-2">
          <button
            onClick={() => setTimeRange(30)}
            className={`px-4 py-2 rounded-md transition-colors ${
              timeRange === 30
                ? 'bg-blue-600 text-white'
                : 'bg-white dark:bg-gray-800 text-gray-700 dark:text-gray-300 hover:bg-gray-100 dark:hover:bg-gray-700'
            }`}
          >
            Last 30 Days
          </button>
          <button
            onClick={() => setTimeRange(90)}
            className={`px-4 py-2 rounded-md transition-colors ${
              timeRange === 90
                ? 'bg-blue-600 text-white'
                : 'bg-white dark:bg-gray-800 text-gray-700 dark:text-gray-300 hover:bg-gray-100 dark:hover:bg-gray-700'
            }`}
          >
            Last 90 Days
          </button>
        </div>

        {isLoading ? (
          <div className="text-center py-12">
            <div className="inline-block animate-spin rounded-full h-12 w-12 border-b-2 border-blue-600"></div>
            <p className="mt-4 text-gray-600 dark:text-gray-400">Loading statistics...</p>
          </div>
        ) : (
          <>
            {/* Stats Overview */}
            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-6 mb-8">
              <div className="bg-white dark:bg-gray-800 rounded-lg shadow p-6">
                <h2 className="text-sm font-medium text-gray-600 dark:text-gray-400 mb-1">
                  Total Users
                </h2>
                <p className="text-3xl font-bold text-gray-900 dark:text-white">
                  {stats?.users.total || 0}
                </p>
                <p className="text-xs text-gray-500 dark:text-gray-500 mt-1">
                  {stats?.users.active7d || 0} active (7d) • {stats?.users.active30d || 0} active (30d)
                </p>
              </div>

              <div className="bg-white dark:bg-gray-800 rounded-lg shadow p-6">
                <h2 className="text-sm font-medium text-gray-600 dark:text-gray-400 mb-1">
                  Total Feeds
                </h2>
                <p className="text-3xl font-bold text-gray-900 dark:text-white">
                  {stats?.feeds.total || 0}
                </p>
                <p className="text-xs text-gray-500 dark:text-gray-500 mt-1">
                  {stats?.feeds.active || 0} active • {stats?.feeds.guideFeeds || 0} in guide
                </p>
              </div>

              <div className="bg-white dark:bg-gray-800 rounded-lg shadow p-6">
                <h2 className="text-sm font-medium text-gray-600 dark:text-gray-400 mb-1">
                  Feed Items
                </h2>
                <p className="text-3xl font-bold text-gray-900 dark:text-white">
                  {stats?.items.total.toLocaleString() || 0}
                </p>
                <p className="text-xs text-gray-500 dark:text-gray-500 mt-1">
                  {stats?.items.read30d || 0} read (30d)
                </p>
              </div>

              <div className="bg-white dark:bg-gray-800 rounded-lg shadow p-6">
                <h2 className="text-sm font-medium text-gray-600 dark:text-gray-400 mb-1">
                  Subscriptions
                </h2>
                <p className="text-3xl font-bold text-gray-900 dark:text-white">
                  {stats?.subscriptions.total || 0}
                </p>
                <p className="text-xs text-gray-500 dark:text-gray-500 mt-1">
                  {stats?.subscriptions.paid || 0} paid • {stats?.subscriptions.trial || 0} trial
                </p>
              </div>
            </div>

            {/* User Growth Chart */}
            <div className="bg-white dark:bg-gray-800 rounded-lg shadow p-6 mb-8">
              <h2 className="text-xl font-semibold text-gray-900 dark:text-white mb-4">
                User Growth (Cumulative)
              </h2>
              <div className="h-64 flex items-end justify-between gap-1">
                {userGrowth?.map((day, idx) => {
                  const maxCount = Math.max(...(userGrowth?.map(d => d.count) || [1]))
                  const height = maxCount > 0 ? (day.count / maxCount) * 100 : 0
                  const isRecent = idx >= userGrowth.length - 7
                  
                  return (
                    <div key={day.date} className="flex-1 flex flex-col items-center group relative">
                      <div 
                        className={`w-full rounded-t transition-all ${
                          isRecent ? 'bg-blue-500' : 'bg-blue-400 dark:bg-blue-600'
                        } hover:bg-blue-600 dark:hover:bg-blue-500`}
                        style={{ height: `${height}%`, minHeight: day.count > 0 ? '2px' : '0' }}
                      />
                      <div className="absolute bottom-full mb-2 hidden group-hover:block bg-gray-900 text-white text-xs rounded py-1 px-2 whitespace-nowrap z-10">
                        {new Date(day.date).toLocaleDateString()}: {day.count} users
                      </div>
                    </div>
                  )
                })}
              </div>
              <div className="flex justify-between text-xs text-gray-500 dark:text-gray-400 mt-2">
                <span>{userGrowth?.[0]?.date ? new Date(userGrowth[0].date).toLocaleDateString() : ''}</span>
                <span>{userGrowth?.[userGrowth.length - 1]?.date ? new Date(userGrowth[userGrowth.length - 1].date).toLocaleDateString() : ''}</span>
              </div>
            </div>

            {/* Activity Chart */}
            <div className="bg-white dark:bg-gray-800 rounded-lg shadow p-6 mb-8">
              <h2 className="text-xl font-semibold text-gray-900 dark:text-white mb-4">
                Daily Activity (Items Read)
              </h2>
              <div className="h-64 flex items-end justify-between gap-1">
                {activity?.map((day, idx) => {
                  const maxReads = Math.max(...(activity?.map(d => d.reads) || [1]))
                  const height = maxReads > 0 ? (day.reads / maxReads) * 100 : 0
                  const isRecent = idx >= activity.length - 7
                  
                  return (
                    <div key={day.date} className="flex-1 flex flex-col items-center group relative">
                      <div 
                        className={`w-full rounded-t transition-all ${
                          isRecent ? 'bg-green-500' : 'bg-green-400 dark:bg-green-600'
                        } hover:bg-green-600 dark:hover:bg-green-500`}
                        style={{ height: `${height}%`, minHeight: day.reads > 0 ? '2px' : '0' }}
                      />
                      <div className="absolute bottom-full mb-2 hidden group-hover:block bg-gray-900 text-white text-xs rounded py-1 px-2 whitespace-nowrap z-10">
                        {new Date(day.date).toLocaleDateString()}: {day.reads} reads
                      </div>
                    </div>
                  )
                })}
              </div>
              <div className="flex justify-between text-xs text-gray-500 dark:text-gray-400 mt-2">
                <span>{activity?.[0]?.date ? new Date(activity[0].date).toLocaleDateString() : ''}</span>
                <span>{activity?.[activity.length - 1]?.date ? new Date(activity[activity.length - 1].date).toLocaleDateString() : ''}</span>
              </div>
            </div>

            {/* Current User Info */}
            <div className="bg-white dark:bg-gray-800 rounded-lg shadow p-6">
              <h2 className="text-xl font-semibold text-gray-900 dark:text-white mb-4">
                Current Session
              </h2>
              <div className="space-y-2 text-sm">
                <p className="text-gray-600 dark:text-gray-400">
                  <span className="font-semibold">npub:</span> {user?.npub}
                </p>
                <p className="text-gray-600 dark:text-gray-400">
                  <span className="font-semibold">pubkey:</span> {user?.pubkey}
                </p>
              </div>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
