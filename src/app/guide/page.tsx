'use client'

import Link from "next/link";
import { useState } from "react";
import { api } from "@/trpc/react";
import { useRouter } from "next/navigation";
import { useNostrAuth } from "@/contexts/NostrAuthContext";
import { nip19 } from "nostr-tools";
import { BrandHeader } from "@/components/brand-header";

export default function GuidePage() {
  const { isConnected, getPublicKey } = useNostrAuth();
  const [selectedTags, setSelectedTags] = useState<string[]>([]);
  const [orderBy, setOrderBy] = useState<'newest' | 'popular' | 'recent_posts'>('popular');
  const [copySuccess, setCopySuccess] = useState<string | null>(null);
  const [editingNpub, setEditingNpub] = useState<string | null>(null);
  const [editTags, setEditTags] = useState<string[]>([]);
  const [editTagInput, setEditTagInput] = useState('');

  const router = useRouter();

  // Fetch guide feeds with filters
  const { data: guideFeeds, isLoading: feedsLoading } = api.guide.getGuideFeeds.useQuery({
    tags: selectedTags.length > 0 ? selectedTags : undefined,
    orderBy,
    limit: 50,
  });

  // Fetch all available tags
  const { data: availableTags } = api.guide.getGuideTags.useQuery();

  const subscribeMutation = api.feed.subscribeFeed.useMutation();
  const incrementSubscriberMutation = api.guide.incrementSubscriberCount.useMutation();
  const updateTagsMutation = api.guide.updateOwnTags.useMutation();
  const deleteEntryMutation = api.guide.deleteOwnEntry.useMutation();

  const utils = api.useUtils();

  const handleToggleTag = (tag: string) => {
    if (selectedTags.includes(tag)) {
      setSelectedTags(selectedTags.filter(t => t !== tag));
    } else {
      setSelectedTags([...selectedTags, tag]);
    }
  };

  const handleCopyRssUrl = (npub: string, tags: string[]) => {
    const tagsParam = tags.length > 0 ? `&tags=${encodeURIComponent(tags.join(','))}` : '';
    const url = `${window.location.origin}/api/nostr-rss?npub=${encodeURIComponent(npub)}${tagsParam}`;
    navigator.clipboard.writeText(url).then(() => {
      setCopySuccess(npub);
      setTimeout(() => setCopySuccess(null), 2000);
    });
  };

  const handleSubscribe = async (npub: string, displayName: string, tags: string[]) => {
    if (!isConnected) {
      router.push('/reader');
      return;
    }

    try {
      await subscribeMutation.mutateAsync({ 
        type: 'NOSTR', 
        npub,
        title: displayName,
        tags: tags, // Pass the tags from the guide feed
      });
      
      // Increment subscriber count in guide
      await incrementSubscriberMutation.mutateAsync({ npub });
      
      router.push('/reader');
    } catch (error: any) {
      console.error('Failed to subscribe:', error);
    }
  };

  const handleStartEdit = (npub: string, currentTags: string[]) => {
    setEditingNpub(npub);
    setEditTags([...currentTags]);
    setEditTagInput('');
  };

  const handleCancelEdit = () => {
    setEditingNpub(null);
    setEditTags([]);
    setEditTagInput('');
  };

  const handleAddEditTag = () => {
    const trimmed = editTagInput.trim();
    if (trimmed && !editTags.includes(trimmed) && editTags.length < 10) {
      setEditTags([...editTags, trimmed]);
      setEditTagInput('');
    }
  };

  const handleRemoveEditTag = (tag: string) => {
    setEditTags(editTags.filter(t => t !== tag));
  };

  const handleSaveTags = async () => {
    if (editTags.length === 0) {
      alert('Please add at least one tag');
      return;
    }

    try {
      await updateTagsMutation.mutateAsync({ tags: editTags });
      await utils.guide.getGuideFeeds.invalidate();
      setEditingNpub(null);
      setEditTags([]);
    } catch (error: any) {
      alert(error.message || 'Failed to update tags');
    }
  };

  const handleDeleteEntry = async (npub: string, displayName: string) => {
    if (!confirm(`Are you sure you want to remove "${displayName}" from the guide? This cannot be undone.`)) {
      return;
    }

    try {
      await deleteEntryMutation.mutateAsync();
      await utils.guide.getGuideFeeds.invalidate();
    } catch (error: any) {
      alert(error.message || 'Failed to delete entry');
    }
  };

  // Helper to check if current user owns this feed entry
  const isOwnEntry = (npub: string) => {
    const currentPubkey = getPublicKey();
    if (!isConnected || !currentPubkey) return false;
    try {
      const { type, data } = nip19.decode(npub);
      return type === 'npub' && data === currentPubkey;
    } catch {
      return false;
    }
  };

  return (
    <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      <BrandHeader cta={{ href: "/guide/submit", label: "Submit a Feed" }} />

      <div className="mx-auto w-full max-w-6xl px-4 py-12">
        {/* Header */}
        <div className="mb-8 rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-6 backdrop-blur-xl md:p-8">
          <div className="flex flex-col gap-4 md:flex-row md:items-center md:justify-between">
            <div>
              <h1 className="text-3xl font-extrabold tracking-tight md:text-4xl">
                <span className="bg-gradient-to-br from-[#27ae60] via-[#2ecc71] to-[#58d68d] bg-clip-text text-transparent">
                  Readstr
                </span>{" "}
                Guide
              </h1>
              <p className="mt-2 text-base leading-relaxed text-[#B3B3B3]">
                Discover long-form content creators on Nostr
              </p>
            </div>
            <Link
              href="/guide/submit"
              className="inline-block rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-8 py-3 text-center text-base font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)]"
            >
              Submit a Feed
            </Link>
          </div>
        </div>

        {/* Filters */}
        <div className="mb-8 rounded-2xl border border-[#27ae60]/15 bg-white/[0.05] p-6 backdrop-blur-xl">
          <div className="mb-5">
            <h2 className="mb-3 text-lg font-semibold text-white">Filter by Tags</h2>
            {availableTags && availableTags.length > 0 ? (
              <div className="flex flex-wrap gap-2">
                {availableTags.map(({ tag, count }) => (
                  <button
                    key={tag}
                    onClick={() => handleToggleTag(tag)}
                    className={`rounded-full px-3 py-1 text-sm font-medium transition-all duration-300 ${
                      selectedTags.includes(tag)
                        ? "bg-gradient-to-br from-[#27ae60] to-[#229954] text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)]"
                        : "border border-[#27ae60]/25 bg-white/10 text-[#B3B3B3] hover:border-[#27ae60]/50 hover:bg-[#27ae60]/10 hover:text-white"
                    }`}
                  >
                    {tag} ({count})
                  </button>
                ))}
              </div>
            ) : (
              <p className="text-[#B3B3B3]">No tags available yet</p>
            )}
          </div>

          <div className="flex items-center gap-4">
            <label htmlFor="orderBy" className="text-sm font-medium text-white">
              Sort by:
            </label>
            <select
              id="orderBy"
              value={orderBy}
              onChange={(e) => setOrderBy(e.target.value as any)}
              className="rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 text-white backdrop-blur-sm transition-colors focus:border-[#27ae60]/60 focus:outline-none focus:ring-2 focus:ring-[#27ae60]/40"
            >
              <option value="popular" className="bg-[#161b22]">Most Popular</option>
              <option value="recent_posts" className="bg-[#161b22]">Recently Posted</option>
              <option value="newest" className="bg-[#161b22]">Newest Feeds</option>
            </select>
          </div>
        </div>

        {/* Feeds List */}
        <div className="space-y-4">
          {feedsLoading ? (
            <div className="rounded-2xl border border-[#27ae60]/15 bg-white/[0.05] p-8 text-center backdrop-blur-xl">
              <p className="text-[#B3B3B3]">Loading feeds...</p>
            </div>
          ) : guideFeeds && guideFeeds.length > 0 ? (
            guideFeeds.map((feed: any) => (
              <div
                key={feed.id}
                className="group rounded-2xl border border-[#27ae60]/15 bg-white/[0.06] p-6 backdrop-blur-xl transition-all duration-300 hover:-translate-y-1 hover:border-[#27ae60]/30 hover:bg-[#27ae60]/[0.08]"
              >
                <div className="flex flex-col gap-4 md:flex-row">
                  {/* Profile Picture */}
                  {feed.picture && (
                    <div className="flex-shrink-0">
                      {/* eslint-disable-next-line @next/next/no-img-element -- remote feed avatar from arbitrary domains, not suited to next/image */}
                      <img
                        src={feed.picture}
                        alt={feed.displayName}
                        className="h-16 w-16 rounded-full border-2 border-[#27ae60]/40 object-cover"
                        onError={(e) => {
                          // Hide image if it fails to load (e.g., Twitter hotlinking blocked)
                          e.currentTarget.style.display = 'none'
                        }}
                      />
                    </div>
                  )}

                  {/* Feed Info */}
                  <div className="flex-grow">
                    <h3 className="mb-1 text-xl font-bold text-white">{feed.displayName}</h3>
                    {feed.about && (
                      <p className="mb-3 line-clamp-2 text-[#B3B3B3]">
                        {feed.about}
                      </p>
                    )}

                    {/* Meta Info */}
                    <div className="mb-3 flex flex-wrap gap-4 text-sm text-[#B3B3B3]">
                      <span>{feed.postCount} posts</span>
                      <span>{feed.subscriberCount} subscribers</span>
                      {feed.lastPublishedAt && (
                        <span>
                          Last post: {new Date(feed.lastPublishedAt).toLocaleDateString()}
                        </span>
                      )}
                    </div>

                    {/* Tags */}
                    {editingNpub === feed.npub ? (
                      <div className="mb-4">
                        <label className="mb-2 block text-sm font-medium text-[#B3B3B3]">
                          Edit Tags
                        </label>
                        <div className="mb-2 flex gap-2">
                          <input
                            type="text"
                            value={editTagInput}
                            onChange={(e) => setEditTagInput(e.target.value)}
                            onKeyDown={(e) => {
                              if (e.key === 'Enter') {
                                e.preventDefault();
                                handleAddEditTag();
                              }
                            }}
                            placeholder="Add a tag..."
                            className="flex-1 rounded-xl border border-[#27ae60]/25 bg-white/10 px-3 py-2 text-white placeholder:text-white/40 focus:border-[#27ae60]/60 focus:outline-none focus:ring-2 focus:ring-[#27ae60]/40"
                          />
                          <button
                            onClick={handleAddEditTag}
                            disabled={editTags.length >= 10}
                            className="rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-4 py-2 font-semibold text-white transition-all duration-300 hover:-translate-y-0.5 disabled:opacity-50"
                          >
                            Add
                          </button>
                        </div>
                        {editTags.length > 0 && (
                          <div className="flex flex-wrap gap-2">
                            {editTags.map((tag) => (
                              <span
                                key={tag}
                                className="inline-flex items-center rounded-full border border-[#27ae60]/40 bg-[#27ae60]/15 px-3 py-1 text-sm text-white"
                              >
                                {tag}
                                <button
                                  onClick={() => handleRemoveEditTag(tag)}
                                  className="ml-2 text-[#B3B3B3] hover:text-white"
                                >
                                  ×
                                </button>
                              </span>
                            ))}
                          </div>
                        )}
                      </div>
                    ) : (
                      feed.tags.length > 0 && (
                        <div className="mb-4 flex flex-wrap gap-2">
                          {feed.tags.map((tag: string) => (
                            <span
                              key={tag}
                              className="rounded border border-[#27ae60]/30 bg-[#27ae60]/10 px-2 py-1 text-xs text-[#B3B3B3]"
                            >
                              {tag}
                            </span>
                          ))}
                        </div>
                      )
                    )}

                    {/* Actions */}
                    <div className="flex flex-wrap gap-2">
                      {editingNpub === feed.npub ? (
                        <>
                          <button
                            onClick={handleSaveTags}
                            disabled={updateTagsMutation.isPending || editTags.length === 0}
                            className="rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-4 py-2 text-sm font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 disabled:opacity-50"
                          >
                            {updateTagsMutation.isPending ? 'Saving...' : 'Save Tags'}
                          </button>
                          <button
                            onClick={handleCancelEdit}
                            className="rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 text-sm font-semibold text-white transition-all duration-300 hover:border-[#27ae60]/50 hover:bg-white/20"
                          >
                            Cancel
                          </button>
                        </>
                      ) : (
                        <>
                          <button
                            onClick={() => handleCopyRssUrl(feed.npub, feed.tags)}
                            className="rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 text-sm font-semibold text-white transition-all duration-300 hover:border-[#27ae60]/50 hover:bg-white/20"
                          >
                            {copySuccess === feed.npub ? 'Copied!' : 'Copy RSS URL'}
                          </button>
                          <button
                            onClick={() => handleSubscribe(feed.npub, feed.displayName, feed.tags)}
                            disabled={subscribeMutation.isPending}
                            className="rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-4 py-2 text-sm font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)] disabled:opacity-50"
                          >
                            {subscribeMutation.isPending ? 'Subscribing...' : 'Subscribe in App'}
                          </button>
                          {isOwnEntry(feed.npub) && (
                            <>
                              <button
                                onClick={() => handleStartEdit(feed.npub, feed.tags)}
                                className="rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 text-sm font-semibold text-white transition-all duration-300 hover:border-[#27ae60]/50 hover:bg-white/20"
                              >
                                Edit Tags
                              </button>
                              <button
                                onClick={() => handleDeleteEntry(feed.npub, feed.displayName)}
                                disabled={deleteEntryMutation.isPending}
                                className="rounded-xl border border-red-500/40 bg-red-500/10 px-4 py-2 text-sm font-semibold text-red-300 transition-all duration-300 hover:bg-red-500/20 disabled:opacity-50"
                              >
                                {deleteEntryMutation.isPending ? 'Deleting...' : 'Delete Entry'}
                              </button>
                            </>
                          )}
                        </>
                      )}
                    </div>
                  </div>
                </div>
              </div>
            ))
          ) : (
            <div className="rounded-2xl border border-[#27ae60]/15 bg-white/[0.05] p-8 text-center backdrop-blur-xl">
              <p className="mb-4 text-[#B3B3B3]">
                {selectedTags.length > 0
                  ? 'No feeds found with the selected tags.'
                  : 'No feeds in the guide yet. Be the first to submit one!'}
              </p>
              <Link
                href="/guide/submit"
                className="inline-block rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-8 py-3 text-base font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)]"
              >
                Submit a Feed
              </Link>
            </div>
          )}
        </div>

        {/* Back Link */}
        <div className="mt-10 text-center">
          <Link href="/" className="text-sm font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]">
            &larr; Back to Home
          </Link>
        </div>
      </div>
    </main>
  );
}
