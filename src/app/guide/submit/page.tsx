'use client'

import Link from "next/link";
import { useState } from "react";
import { api } from "@/trpc/react";
import { useRouter } from "next/navigation";
import { useNostrAuth } from "@/contexts/NostrAuthContext";
import { BrandHeader } from "@/components/brand-header";

export default function SubmitToGuidePage() {
  const { isConnected } = useNostrAuth();
  const [npub, setNpub] = useState('');
  const [tagInput, setTagInput] = useState('');
  const [tags, setTags] = useState<string[]>([]);
  const [error, setError] = useState('');
  const [success, setSuccess] = useState(false);
  const [showLoginPrompt, setShowLoginPrompt] = useState(false);

  const router = useRouter();
  const submitMutation = api.guide.submitFeed.useMutation();

  const handleAddTag = () => {
    const newTag = tagInput.trim().toLowerCase();
    if (newTag && !tags.includes(newTag) && tags.length < 10) {
      setTags([...tags, newTag]);
      setTagInput('');
    }
  };

  const handleRemoveTag = (tagToRemove: string) => {
    setTags(tags.filter(tag => tag !== tagToRemove));
  };

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setError('');
    setSuccess(false);

    if (!npub.trim() || !npub.startsWith('npub1')) {
      setError('Please enter a valid Nostr npub.');
      return;
    }

    if (tags.length === 0) {
      setError('Please add at least one tag to categorize this feed.');
      return;
    }

    try {
      await submitMutation.mutateAsync({ npub, tags });
      setSuccess(true);
      setNpub('');
      setTags([]);

      // Redirect to guide after 2 seconds
      setTimeout(() => {
        router.push('/guide');
      }, 2000);
    } catch (err: any) {
      setError(err.message || 'Failed to submit feed to the guide.');
    }
  };

  return (
    <main className="font-brand relative flex min-h-screen flex-col bg-gradient-to-br from-[#1a1a1a] via-[#0d1117] to-[#161b22] text-white">
      <BrandHeader cta={{ href: "/guide", label: "Back to Guide" }} />

      <div className="mx-auto flex w-full max-w-2xl flex-col px-4 py-12">
        <div className="rounded-2xl border border-[#27ae60]/20 bg-white/[0.06] p-6 backdrop-blur-xl md:p-8">
          <h1 className="mb-3 text-center text-3xl font-extrabold tracking-tight md:text-4xl">
            Submit a Feed to the{" "}
            <span className="bg-gradient-to-br from-[#27ae60] via-[#2ecc71] to-[#58d68d] bg-clip-text text-transparent">
              Readstr
            </span>{" "}
            Guide
          </h1>
          <p className="mb-8 text-center text-base leading-relaxed text-[#B3B3B3]">
            Add a Nostr user with long-form content to the public guide directory
          </p>

          <form onSubmit={handleSubmit} className="space-y-6">
            <div>
              <label htmlFor="npub" className="mb-1 block text-sm font-medium text-white">
                Nostr Public Key (npub) <span className="text-[#27ae60]">*</span>
              </label>
              <input
                type="text"
                id="npub"
                name="npub"
                value={npub}
                onChange={(e) => setNpub(e.target.value)}
                placeholder="npub1..."
                className="w-full rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 text-white placeholder:text-white/40 backdrop-blur-sm transition-colors focus:border-[#27ae60]/60 focus:outline-none focus:ring-2 focus:ring-[#27ae60]/40"
                disabled={submitMutation.isPending}
              />
              <p className="mt-1 text-sm text-[#B3B3B3]">
                This user must have published long-form content (NIP-23).
              </p>
            </div>

            <div>
              <label htmlFor="tags" className="mb-1 block text-sm font-medium text-white">
                Tags <span className="text-[#27ae60]">*</span> (1-10 tags)
              </label>
              <div className="flex gap-2">
                <input
                  type="text"
                  id="tags"
                  name="tags"
                  value={tagInput}
                  onChange={(e) => setTagInput(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') {
                      e.preventDefault();
                      handleAddTag();
                    }
                  }}
                  placeholder="e.g., bitcoin, philosophy, technology"
                  className="flex-1 rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 text-white placeholder:text-white/40 backdrop-blur-sm transition-colors focus:border-[#27ae60]/60 focus:outline-none focus:ring-2 focus:ring-[#27ae60]/40"
                  disabled={submitMutation.isPending || tags.length >= 10}
                />
                <button
                  type="button"
                  onClick={handleAddTag}
                  className="rounded-xl border border-[#27ae60]/25 bg-white/10 px-4 py-2 font-semibold text-white transition-all duration-300 hover:-translate-y-0.5 hover:border-[#27ae60]/50 hover:bg-white/20 disabled:opacity-50"
                  disabled={submitMutation.isPending || tags.length >= 10 || !tagInput.trim()}
                >
                  Add
                </button>
              </div>

              {tags.length > 0 && (
                <div className="mt-3 flex flex-wrap gap-2">
                  {tags.map((tag) => (
                    <span
                      key={tag}
                      className="inline-flex items-center gap-2 rounded-full border border-[#27ae60]/40 bg-[#27ae60]/15 px-3 py-1 text-sm text-white"
                    >
                      {tag}
                      <button
                        type="button"
                        onClick={() => handleRemoveTag(tag)}
                        className="text-[#B3B3B3] hover:text-white"
                        disabled={submitMutation.isPending}
                      >
                        ×
                      </button>
                    </span>
                  ))}
                </div>
              )}
              <p className="mt-1 text-sm text-[#B3B3B3]">
                Add relevant topic tags to help people discover this feed.
              </p>
            </div>

            {error && (
              <div className="rounded-xl border border-red-500/40 bg-red-500/10 p-4 text-red-300">
                <p>{error}</p>
              </div>
            )}

            {success && (
              <div className="rounded-xl border border-[#27ae60]/40 bg-[#27ae60]/10 p-4 text-[#58d68d]">
                <p>Feed successfully submitted to the guide! Redirecting...</p>
              </div>
            )}

            <button
              type="submit"
              disabled={submitMutation.isPending || tags.length === 0}
              className="w-full rounded-xl bg-gradient-to-br from-[#27ae60] to-[#229954] px-4 py-3 text-base font-semibold text-white shadow-[0_4px_14px_rgba(39,174,96,0.25)] transition-all duration-300 hover:-translate-y-0.5 hover:shadow-[0_8px_25px_rgba(39,174,96,0.35)] disabled:opacity-50"
            >
              {submitMutation.isPending ? 'Submitting...' : 'Submit to Guide'}
            </button>
          </form>

          <div className="mt-8 text-center">
            <Link href="/guide" className="text-sm font-medium text-[#27ae60] transition-colors hover:text-[#2ecc71]">
              &larr; Back to Guide
            </Link>
          </div>
        </div>
      </div>
    </main>
  );
}
