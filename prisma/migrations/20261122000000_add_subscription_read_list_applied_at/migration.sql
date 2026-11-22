-- AlterTable
ALTER TABLE "Subscription" ADD COLUMN "readListAppliedAt" TIMESTAMP(3);

-- Existing subscriptions count as covered: the pulled read list was already
-- applied to them, and applying it again could mark read items the user has
-- since marked unread on purpose. Only subscriptions created from now on start
-- out NULL and get the list applied once.
UPDATE "Subscription" SET "readListAppliedAt" = NOW();
