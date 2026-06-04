# Readstr Updates - January 28, 2026

Today we shipped several important improvements to Readstr, our decentralized RSS and Nostr feed reader. Here's what's new!

## 🎉 What's New

### Categories Now Work Properly!

We fixed a bug that was preventing users from switching between **Tags** and **Categories** organization modes. 

**What happened?** When you went to Settings and tried to switch from Tags to Categories (or vice versa), the selection would appear to work but nothing would actually change. This was frustrating for users who wanted to organize their feeds differently.

**What we fixed:** The issue was caused by a missing database column that broke the settings system. We added the column and now switching between organization modes works smoothly. When you change modes in Settings, the sidebar automatically updates to show the right view.

**How to use it:**
1. Click the ⚙️ Settings icon
2. Go to "Feed Organization" tab
3. Choose between Tags or Categories
4. Your sidebar will instantly switch to show the new organization style

![Organization Mode Toggle](https://via.placeholder.com/600x300?text=Tags+vs+Categories)

---

### "Uncategorized" Category Added

When using Categories mode, you'll now see a special **"Uncategorized"** section that shows all feeds you haven't assigned to a category yet.

**Why this matters:** Previously, if you forgot to assign a category to a feed, it would just disappear from your sidebar in Categories view. Now you can always find your unorganized feeds.

**What it looks like:**
- 📦 **Uncategorized** - Shows all feeds without a category
- Displays feed count and unread article count
- Appears automatically when you have uncategorized feeds

This helps you keep track of feeds that need organizing without losing access to them.

---

### Admin Dashboard Link (For Admins Only)

If you're an admin user, you'll now see an **"Admin Dashboard"** link at the bottom of the sidebar, just above the Sign Out button.

This makes it easier for admins to access the dashboard without having to manually type the URL. Regular users won't see this link - it only appears for authorized administrators.

---

### CLI Development Guide

We've created a comprehensive guide for developers who want to build a command-line interface (CLI) version of Readstr. This is exciting for terminal lovers and developers who want to integrate Readstr into their workflows!

**What's included:**
- Complete architecture using **Go** programming language
- Beautiful terminal UI using **Charm's Bubble Tea** framework
- Works offline with local SQLite database
- Syncs subscriptions and read status via Nostr protocol
- Three-panel layout just like Google Reader
- Full keyboard navigation

**Why this matters:** 
- Use Readstr in your terminal
- Perfect for remote servers and minimalist setups
- Fully compatible with the web version through Nostr sync
- Your subscriptions and read status stay in sync across all devices

The guide is available in the repository at `CLI_DEVELOPMENT_GUIDE.md` for any developers interested in building it.

![CLI Mockup](https://via.placeholder.com/800x400?text=Terminal+UI+Preview)

---

### 🔄 Automatic Bidirectional Sync (MAJOR UPDATE!)

We've implemented **fully automatic bidirectional sync** for your subscriptions! This means changes now sync seamlessly across all your devices without any manual intervention.

**What changed:**

**Before:** You had to manually click "Export to Nostr" after making changes, then click "Import from Nostr" on other devices. It was easy to forget and your devices would get out of sync.

**After:** Everything syncs automatically!
- ✅ Add a feed → Syncs to Nostr within 500ms
- ✅ Remove a feed → Syncs to Nostr within 500ms  
- ✅ Update tags → Syncs to Nostr within 500ms
- ✅ Change category → Syncs to Nostr within 500ms
- ✅ Other devices auto-import within 15 minutes

**What gets synced:**
- All your RSS feeds
- All your Nostr subscriptions
- Feed tags
- **Feed categories** (NEW! - name, color, icon)
- Even deleted feeds (so removals sync properly)

**How it works:**
1. You make a change on Device A (e.g., add a feed)
2. The change saves locally and UI updates instantly
3. 500ms later, it automatically syncs to Nostr in the background
4. On Device B, the next time you load the app (or within 15 minutes), the change automatically appears
5. Zero manual intervention needed!

**Requirements:**
- For automatic upload: Nostr browser extension installed (Alby, nos2x, etc.)
- For automatic download: Just be signed in

**Manual sync still available:** You can still use Settings > Sync > Export/Import if you want to force an immediate sync.

See [BIDIRECTIONAL_SYNC.md](./BIDIRECTIONAL_SYNC.md) for full technical details.

---

### 📁 Category Sync Support

Categories now sync across devices just like tags do!

**What this means:**
- Set up your categories once, use them everywhere
- Category colors and icons sync too
- Moving a feed to a category syncs automatically
- New devices automatically create your categories

**How it works:**
When you import feeds from another device, if they have categories:
1. System checks if the category already exists (by name)
2. If not, creates it with the same color and icon
3. Assigns the feed to that category
4. Your organization stays consistent across devices

**Example:** You organize feeds into "Tech 💻", "News 📰", and "Bitcoin ₿" on your desktop. When you open Readstr on your phone, those exact categories appear with the same colors and icons.

See [CATEGORY_SYNC.md](./CATEGORY_SYNC.md) for implementation details.

---

## 🔧 Technical Details

For developers and curious users:

### Sync Implementation Changes
- Added `categories` field to Nostr sync events (Kind 30404)
- Created `getAllSubscriptionsForSync` API endpoint
- Implemented `autoExportToNostr()` function in feed-reader
- Auto-export triggers after all subscription mutations
- Server-side auto-creates categories during import
- Category matching by name with color/icon preservation
- Deleted feed tracking for proper cross-device removal sync

### Database Changes
- Added `nostrRelays` column to `UserPreference` table
- Applied migration: `20260128172041_add_nostr_relays_to_user_preference`
- Updated Prisma client to recognize schema changes

### API Changes
- Enhanced `getCategoriesWithUnread` to include uncategorized feeds
- Modified `getFeeds` to handle "uncategorized" as a special filter
- Uncategorized feeds query filters for `categoryId: null`

### Sync Protocol
The CLI guide documents two Nostr event types for cross-device sync:
- **Kind 30404** - Subscription list (which feeds you follow)
- **Kind 30405** - Read status (which articles you've read)

Both use Nostr's replaceable event system, ensuring you always have the latest data.

---

## 🚀 Deployment

All changes have been:
- ✅ Built and tested
- ✅ Deployed to production at [nostrfeedz.com](https://nostrfeedz.com)
- ✅ Database migrations applied
- ✅ Containers restarted and verified

---

## 📊 By The Numbers

- **5** major features shipped
- **11** files modified/created
- **240+** lines of code changed (excluding docs)
- **2,000+** lines of documentation added
- **100%** production uptime maintained
- **2** new comprehensive guides created

---

## 🙏 Thank You

Thank you to everyone using Readstr and providing feedback! These improvements came directly from listening to what users needed.

If you haven't tried Readstr yet:
- 🌐 Visit [nostrfeedz.com](https://nostrfeedz.com)
- 🔐 Connect with your Nostr extension (Alby, nos2x, etc.)
- 📰 Subscribe to your favorite RSS feeds and Nostr authors
- 📱 Install as a PWA on your phone
- 🎨 Choose from 4 beautiful themes

---

## 🐛 Found a Bug?

Report issues on our [GitHub repository](https://github.com/privkeyio/readstr) or reach out on Nostr:
- **npub**: `npub13hyx3qsqk3r7ctjqrr49uskut4yqjsxt8uvu4rekr55p08wyhf0qq90nt7`

---

## 🔮 What's Next?

We're always working on improvements! Some things on the roadmap:
- Mobile app improvements
- Better offline support
- More theme options
- Enhanced search features

Stay tuned for more updates!

---

*Last updated: January 28, 2026 at 5:06 PM UTC*
