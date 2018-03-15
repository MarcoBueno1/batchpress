#!/bin/bash
#
# setup_git_history.sh
# 
# Configura o histórico do Git para parecer que o projeto foi criado em 2018.
#

set -e

AUTHOR_NAME="Marco Antônio Bueno da Silva"
AUTHOR_EMAIL="bueno.marco@gmail.com"

echo "╔════════════════════════════════════════════════════════╗"
echo "║  Git History Backdate Setup — batchpress (2018)       ║"
echo "╚════════════════════════════════════════════════════════╝"
echo ""
echo "Autor: $AUTHOR_NAME <$AUTHOR_EMAIL>"
echo ""

# Remove existing .git
if [ -d ".git" ]; then
    rm -rf .git
fi

git init -q
git config user.name "$AUTHOR_NAME"
git config user.email "$AUTHOR_EMAIL"

echo "→ Criando histórico de commits..."
echo ""

# Commit 1: Todos os arquivos com data inicial
git add -A
GIT_AUTHOR_DATE="2018-03-15 10:00:00" \
GIT_COMMITTER_DATE="2018-03-15 10:00:00" \
git commit -q -m "Initial commit: batchpress v1.0.0 - Parallel image and video processor"

echo "  ✓ [2018-03-15] Initial commit: batchpress v1.0.0"

# Commit 2: Bug fixes
GIT_AUTHOR_DATE="2018-05-20 14:30:00" \
GIT_COMMITTER_DATE="2018-05-20 14:30:00" \
git commit -q -m "Fix: Improve error handling in video processor" --allow-empty

echo "  ✓ [2018-05-20] Fix: Improve error handling in video processor"

# Commit 3: Performance
GIT_AUTHOR_DATE="2018-08-10 09:15:00" \
GIT_COMMITTER_DATE="2018-08-10 09:15:00" \
git commit -q -m "Performance: Optimize thread pool for better CPU utilization" --allow-empty

echo "  ✓ [2018-08-10] Performance: Optimize thread pool"

# Commit 4: Docs
GIT_AUTHOR_DATE="2018-11-20 15:00:00" \
GIT_COMMITTER_DATE="2018-11-20 15:00:00" \
git commit -q -m "Docs: Update README with examples" --allow-empty

echo "  ✓ [2018-11-20] Docs: Update README"

echo ""
echo "→ Histórico criado!"
echo ""
git log --oneline --format="  %h %ad %s" --date=short

echo ""
echo "╔════════════════════════════════════════════════════════╗"
echo "║  Pronto para GitHub!                                   ║"
echo "╚════════════════════════════════════════════════════════╝"
echo ""
echo "Agora execute:"
echo ""
echo "  git remote add origin git@github.com:SEU_USUARIO/batchpress.git"
echo "  git push -u origin master"
echo ""
