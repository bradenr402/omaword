# bash completion for omaword                               -*- shell-script -*-
# `omaword completion bash` prints it; bin/install installs it.

# A group's commands.
_omaword_subcommands()
{
    case $1 in
        books) echo "list outline" ;;
        marks) echo "list show export backup" ;;
        tags | history) echo "list" ;;
        desk | ribbon) echo "show" ;;
        translations | versions) echo "list show install remove import" ;;
        catalog) echo "list show refresh" ;;
    esac
}

_omaword()
{
    local cur=${COMP_WORDS[COMP_CWORD]} prev=${COMP_WORDS[COMP_CWORD-1]}
    local commands="open lookup read quote refs search link books marks tags desk history ribbon translations catalog completion help"
    case $prev in
        -t | --translation)
            COMPREPLY=($(compgen -W "$(omaword translations --names 2>/dev/null)" -- "$cur"))
            return ;;
        --color)
            COMPREPLY=($(compgen -W "auto always never" -- "$cur"))
            return ;;
        --in)
            COMPREPLY=($(compgen -W "ot nt apocrypha" -- "$cur"))
            return ;;
        --limit | --tag | --language)
            return ;;
        --import | -o | --output)
            compopt -o filenames 2>/dev/null
            COMPREPLY=($(compgen -f -- "$cur"))
            return ;;
    esac
    if ((COMP_CWORD == 1)); then
        COMPREPLY=($(compgen -W "$commands --help --version" -- "$cur"))
        return
    fi
    local command=${COMP_WORDS[1]} subcommands
    subcommands=$(_omaword_subcommands "$command")
    if [[ -n $subcommands ]]; then
        if ((COMP_CWORD == 2)) && [[ $cur != -* ]]; then
            COMPREPLY=($(compgen -W "$subcommands" -- "$cur"))
            return
        fi
        # The group's default command (listed first), unless another is named.
        if [[ " $subcommands " == *" ${COMP_WORDS[2]} "* ]]; then command+=" ${COMP_WORDS[2]}"; else command+=" ${subcommands%% *}"; fi
    fi
    local options words files
    case $command in
        open) options="--here --translation --help" ;;
        lookup) options="--translation --help" ;;
        read) options="--footnotes --json --color --translation --help" ;;
        quote) options="--all --plain --json --translation --help" ;;
        refs) options="--quote --limit --json --color --translation --help" ;;
        search) options="--all --in --limit --json --color --translation --help" ;;
        link) options="--translation --help" ;;
        "books list" | "books outline" | "marks show") options="--json --translation --help" ;;
        "marks list") options="--tag --notes --highlights --all --json --translation --help" ;;
        "marks export") options="--output --help" ;;
        "marks backup" | "translations import" | "versions import" | import) options="--help" files=1 ;;
        "tags list" | "history list" | "ribbon show" | "catalog show") options="--json --help" ;;
        "desk show" | "catalog refresh" | "translations install" | "versions install") options="--help" ;;
        "catalog list") options="--language --all-languages --json --help" ;;
        "translations list" | "versions list") options="--names --json --help" ;;
        "translations show" | "versions show") options="--json --help" words=$(omaword translations --names 2>/dev/null) ;;
        "translations remove" | "versions remove") options="--delete-marks --help" words=$(omaword translations --names 2>/dev/null) ;;
        completion) ((COMP_CWORD == 2)) && words="bash zsh fish" ;;
        help) ((COMP_CWORD == 2)) && words="$commands"
              ((COMP_CWORD == 3)) && words=$(_omaword_subcommands "${COMP_WORDS[2]}") ;;
    esac
    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W "$options" -- "$cur"))
    elif [[ -n $files ]]; then
        compopt -o filenames 2>/dev/null
        COMPREPLY=($(compgen -f -- "$cur"))
    else
        COMPREPLY=($(compgen -W "$words" -- "$cur"))
    fi
}
complete -F _omaword omaword
