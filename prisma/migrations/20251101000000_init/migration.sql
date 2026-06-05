-- CreateEnum
CREATE TYPE "SubscriptionStatus" AS ENUM ('TRIAL', 'ACTIVE', 'PAST_DUE', 'CANCELLED', 'EXPIRED');

-- CreateEnum
CREATE TYPE "FeedType" AS ENUM ('RSS', 'NOSTR', 'VIDEO');

-- CreateTable
CREATE TABLE "Feed" (
    "id" TEXT NOT NULL,
    "type" "FeedType" NOT NULL,
    "url" TEXT,
    "npub" TEXT,
    "title" TEXT NOT NULL,
    "description" TEXT,
    "isActive" BOOLEAN NOT NULL DEFAULT true,
    "lastFetchedAt" TIMESTAMP(3),
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "Feed_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "FeedItem" (
    "id" TEXT NOT NULL,
    "feedId" TEXT NOT NULL,
    "title" TEXT NOT NULL,
    "content" TEXT NOT NULL,
    "url" TEXT,
    "author" TEXT,
    "publishedAt" TIMESTAMP(3) NOT NULL,
    "guid" TEXT,
    "videoId" TEXT,
    "embedUrl" TEXT,
    "thumbnail" TEXT,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "FeedItem_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "Subscription" (
    "id" TEXT NOT NULL,
    "userPubkey" TEXT NOT NULL,
    "feedId" TEXT NOT NULL,
    "tags" TEXT[] DEFAULT ARRAY[]::TEXT[],
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "Subscription_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "ReadItem" (
    "id" TEXT NOT NULL,
    "userPubkey" TEXT NOT NULL,
    "itemId" TEXT NOT NULL,
    "readAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,

    CONSTRAINT "ReadItem_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "NostrRelay" (
    "id" SERIAL NOT NULL,
    "url" TEXT NOT NULL,
    "name" TEXT,
    "isActive" BOOLEAN NOT NULL DEFAULT true,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "NostrRelay_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "GuideFeed" (
    "id" TEXT NOT NULL,
    "npub" TEXT NOT NULL,
    "displayName" TEXT NOT NULL,
    "about" TEXT,
    "picture" TEXT,
    "tags" TEXT[] DEFAULT ARRAY[]::TEXT[],
    "submittedBy" TEXT,
    "lastPublishedAt" TIMESTAMP(3),
    "postCount" INTEGER NOT NULL DEFAULT 0,
    "subscriberCount" INTEGER NOT NULL DEFAULT 0,
    "isActive" BOOLEAN NOT NULL DEFAULT true,
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "GuideFeed_pkey" PRIMARY KEY ("id")
);

-- CreateTable
CREATE TABLE "UserSubscription" (
    "id" TEXT NOT NULL,
    "userPubkey" TEXT NOT NULL,
    "status" "SubscriptionStatus" NOT NULL DEFAULT 'TRIAL',
    "trialEndsAt" TIMESTAMP(3) NOT NULL,
    "subscriptionEndsAt" TIMESTAMP(3),
    "squareCustomerId" TEXT,
    "squareSubscriptionId" TEXT,
    "cancelledAt" TIMESTAMP(3),
    "createdAt" TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP,
    "updatedAt" TIMESTAMP(3) NOT NULL,

    CONSTRAINT "UserSubscription_pkey" PRIMARY KEY ("id")
);

-- CreateIndex
CREATE INDEX "Feed_type_isActive_idx" ON "Feed"("type", "isActive");

-- CreateIndex
CREATE UNIQUE INDEX "Feed_type_url_key" ON "Feed"("type", "url");

-- CreateIndex
CREATE UNIQUE INDEX "Feed_type_npub_key" ON "Feed"("type", "npub");

-- CreateIndex
CREATE INDEX "FeedItem_feedId_publishedAt_idx" ON "FeedItem"("feedId", "publishedAt");

-- CreateIndex
CREATE UNIQUE INDEX "FeedItem_feedId_guid_key" ON "FeedItem"("feedId", "guid");

-- CreateIndex
CREATE INDEX "Subscription_userPubkey_idx" ON "Subscription"("userPubkey");

-- CreateIndex
CREATE INDEX "Subscription_userPubkey_tags_idx" ON "Subscription"("userPubkey", "tags");

-- CreateIndex
CREATE UNIQUE INDEX "Subscription_userPubkey_feedId_key" ON "Subscription"("userPubkey", "feedId");

-- CreateIndex
CREATE INDEX "ReadItem_userPubkey_idx" ON "ReadItem"("userPubkey");

-- CreateIndex
CREATE UNIQUE INDEX "ReadItem_userPubkey_itemId_key" ON "ReadItem"("userPubkey", "itemId");

-- CreateIndex
CREATE UNIQUE INDEX "NostrRelay_url_key" ON "NostrRelay"("url");

-- CreateIndex
CREATE UNIQUE INDEX "GuideFeed_npub_key" ON "GuideFeed"("npub");

-- CreateIndex
CREATE INDEX "GuideFeed_tags_idx" ON "GuideFeed"("tags");

-- CreateIndex
CREATE INDEX "GuideFeed_lastPublishedAt_idx" ON "GuideFeed"("lastPublishedAt");

-- CreateIndex
CREATE INDEX "GuideFeed_subscriberCount_idx" ON "GuideFeed"("subscriberCount");

-- CreateIndex
CREATE UNIQUE INDEX "UserSubscription_userPubkey_key" ON "UserSubscription"("userPubkey");

-- CreateIndex
CREATE INDEX "UserSubscription_userPubkey_idx" ON "UserSubscription"("userPubkey");

-- CreateIndex
CREATE INDEX "UserSubscription_status_idx" ON "UserSubscription"("status");

-- AddForeignKey
ALTER TABLE "FeedItem" ADD CONSTRAINT "FeedItem_feedId_fkey" FOREIGN KEY ("feedId") REFERENCES "Feed"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "Subscription" ADD CONSTRAINT "Subscription_feedId_fkey" FOREIGN KEY ("feedId") REFERENCES "Feed"("id") ON DELETE CASCADE ON UPDATE CASCADE;

-- AddForeignKey
ALTER TABLE "ReadItem" ADD CONSTRAINT "ReadItem_itemId_fkey" FOREIGN KEY ("itemId") REFERENCES "FeedItem"("id") ON DELETE CASCADE ON UPDATE CASCADE;

