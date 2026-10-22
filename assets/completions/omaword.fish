# fish completion for omaword: `omaword completion fish > ~/.config/fish/completions/omaword.fish`.

set -l commands open lookup read quote refs search link books marks tags desk history ribbon translations catalog import completion help
set -l given $commands versions
complete -c omaword -f
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a open -d 'Open a passage in a window'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a lookup -d 'Look up a passage in a small window'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a read -d 'Print passages as the reader lays them out'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a quote -d 'Print passages as quotations'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a refs -d 'Print cross-references and passages tagged alike'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a search -d 'Search the Bible'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a link -d 'Print an omaword: link to a passage'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a books -d "List a translation's books, or a book's outline"
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a marks -d 'List your highlights, notes, and tags'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a tags -d 'List your tags'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a desk -d 'Print the Desk'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a history -d 'List the passages you visited'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a ribbon -d 'Print where the ribbon lies'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a translations -d 'List the installed translations, or show one'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a catalog -d "Browse AO Lab's catalog of translations"
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a import -d 'Install a translation from AO complete JSON'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a completion -d 'Print a shell completion script'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -a help -d 'Show help for OmaWord or a command'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -s h -l help -d 'Show help'
complete -c omaword -n "not __fish_seen_subcommand_from $given" -s v -l version -d "Show OmaWord's version"
# A group's commands follow its name (after help too).
complete -c omaword -n "__fish_seen_subcommand_from books; and not __fish_seen_subcommand_from list outline" -a list -d "List the translation's books"
complete -c omaword -n "__fish_seen_subcommand_from books; and not __fish_seen_subcommand_from list outline" -a outline -d "Print a book's section headings"
complete -c omaword -n "__fish_seen_subcommand_from marks; and not __fish_seen_subcommand_from list show export backup" -a list -d 'List your marks'
complete -c omaword -n "__fish_seen_subcommand_from marks; and not __fish_seen_subcommand_from list show export backup" -a show -d 'Print the marks on a passage'
complete -c omaword -n "__fish_seen_subcommand_from marks; and not __fish_seen_subcommand_from list show export backup" -a export -d 'Print your marks as Markdown'
complete -c omaword -n "__fish_seen_subcommand_from marks; and not __fish_seen_subcommand_from list show export backup" -a backup -d 'Back up your marks as JSON'
complete -c omaword -n "__fish_seen_subcommand_from export" -s o -l output -r -F -d 'Write them to this file'
complete -c omaword -n "__fish_seen_subcommand_from backup" -F
complete -c omaword -n "__fish_seen_subcommand_from tags history; and not __fish_seen_subcommand_from list" -a list -d 'List them'
complete -c omaword -n "__fish_seen_subcommand_from desk ribbon; and not __fish_seen_subcommand_from show" -a show -d 'Print it'
complete -c omaword -n "__fish_seen_subcommand_from catalog; and not __fish_seen_subcommand_from list show refresh" -a list -d 'List the translations AO Lab offers'
complete -c omaword -n "__fish_seen_subcommand_from catalog; and not __fish_seen_subcommand_from list show refresh" -a show -d "Show a translation in AO Lab's catalog"
complete -c omaword -n "__fish_seen_subcommand_from catalog; and not __fish_seen_subcommand_from list show refresh" -a refresh -d "Download AO Lab's current catalog"
complete -c omaword -n "__fish_seen_subcommand_from catalog" -l language -x -d "This language's, by its ISO 639-3 code"
complete -c omaword -n "__fish_seen_subcommand_from catalog" -l all-languages -d "Every language's"
complete -c omaword -n "__fish_seen_subcommand_from translations versions; and not __fish_seen_subcommand_from list show" -a list -d 'List the installed translations'
complete -c omaword -n "__fish_seen_subcommand_from translations versions; and not __fish_seen_subcommand_from list show" -a show -d "Show a translation's source and rights"
complete -c omaword -n "__fish_seen_subcommand_from translations versions; and __fish_seen_subcommand_from show" -x -a '(omaword translations --names 2>/dev/null)'
complete -c omaword -n "__fish_seen_subcommand_from open lookup read quote refs search link books marks" -s t -l translation -x -a '(omaword translations --names 2>/dev/null)' -d 'Use a translation'
complete -c omaword -n "__fish_seen_subcommand_from open" -l here -d 'In the window you were reading'
complete -c omaword -n "__fish_seen_subcommand_from read" -l footnotes -d 'The footnotes after each passage'
complete -c omaword -n "__fish_seen_subcommand_from quote" -l plain -d 'The text alone'
complete -c omaword -n "__fish_seen_subcommand_from refs" -l quote -d 'Each passage in full'
complete -c omaword -n "__fish_seen_subcommand_from read quote refs search books marks tags history ribbon translations versions catalog" -l json -d 'JSON output'
complete -c omaword -n "__fish_seen_subcommand_from quote search marks" -l all -d 'Every installed translation'
complete -c omaword -n "__fish_seen_subcommand_from marks" -l tag -x -d 'Only marks with a tag starting so'
complete -c omaword -n "__fish_seen_subcommand_from marks" -l notes -d 'Only marks with a note'
complete -c omaword -n "__fish_seen_subcommand_from marks" -l highlights -d 'Only highlighted marks'
complete -c omaword -n "__fish_seen_subcommand_from search" -l in -x -a 'ot nt apocrypha' -d 'Only these books: ot, nt, apocrypha, or a book'
complete -c omaword -n "__fish_seen_subcommand_from refs search" -l limit -x -d 'At most this many (0: all)'
complete -c omaword -n "__fish_seen_subcommand_from read refs search" -l color -x -a 'auto always never' -d 'When to color'
complete -c omaword -n "__fish_seen_subcommand_from translations versions" -l names -d 'Abbreviations only'
complete -c omaword -n "__fish_seen_subcommand_from import" -F
complete -c omaword -n "__fish_seen_subcommand_from completion" -x -a 'bash zsh fish'
complete -c omaword -n "__fish_seen_subcommand_from help; and not __fish_seen_subcommand_from $commands[1..-2]" -x -a "$commands"
