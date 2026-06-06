import { PrismaClient, type Prisma } from '@prisma/client'

const globalForPrisma = globalThis as unknown as {
  prisma: PrismaClient | undefined
}

const log: Prisma.LogLevel[] =
  process.env.NODE_ENV === 'production'
    ? ['warn', 'error']
    : ['query', 'warn', 'error']

export const db =
  globalForPrisma.prisma ??
  new PrismaClient({
    log,
  })

if (process.env.NODE_ENV !== 'production') globalForPrisma.prisma = db