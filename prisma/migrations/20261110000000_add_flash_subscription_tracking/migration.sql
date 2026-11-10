-- AlterTable
ALTER TABLE "UserSubscription" ADD COLUMN "flashSubscriptionId" TEXT,
ADD COLUMN "flashEventAt" TIMESTAMP(3);

-- CreateIndex
CREATE UNIQUE INDEX "UserSubscription_flashSubscriptionId_key" ON "UserSubscription"("flashSubscriptionId");
