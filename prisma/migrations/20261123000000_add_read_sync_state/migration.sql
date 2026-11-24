-- CreateTable
CREATE TABLE "ReadSyncState" (
    "userPubkey" TEXT NOT NULL,
    "readListAppliedCreatedAt" BIGINT NOT NULL,

    CONSTRAINT "ReadSyncState_pkey" PRIMARY KEY ("userPubkey")
);

-- No rows to start with: a user without one has nothing recorded, and their
-- browsers fall back to their own watermark until one applies or publishes a
-- read list.
