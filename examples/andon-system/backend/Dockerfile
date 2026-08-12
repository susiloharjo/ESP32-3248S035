# Minimal test backend - see src/server.ts's header comment for scope
# (in-memory only, no auth/DB - do not use this image beyond local dev/test).

FROM node:20-slim AS build
WORKDIR /app
COPY package.json package-lock.json* ./
RUN npm install
COPY tsconfig.json ./
COPY src ./src
RUN npm run build

FROM node:20-slim
WORKDIR /app
ENV NODE_ENV=production
COPY package.json package-lock.json* ./
RUN npm install --omit=dev
COPY --from=build /app/dist ./dist

EXPOSE 8080 1883
CMD ["node", "dist/server.js"]
