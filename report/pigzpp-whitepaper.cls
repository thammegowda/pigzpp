%% pigzpp-whitepaper.cls
%% A small two-column "technical whitepaper" class for the pigzpp report.
%% It bundles the fonts, palette, section styling, macros, and an ACL-style
%% natbib bibliography so that main.tex stays short and human-readable.
%%
%% Fonts are loaded by relative path from ./fonts/ (fetched by ./setup.sh),
%% which keeps Tectonic/XeTeX builds fully reproducible and warning-free.

\NeedsTeXFormat{LaTeX2e}
\ProvidesClass{pigzpp-whitepaper}[2026/07/14 pigzpp technical whitepaper class]

% Pass any extra class options through to article, then load it two-column.
\DeclareOption*{\PassOptionsToClass{\CurrentOption}{article}}
\ProcessOptions\relax
\LoadClass[10pt,twocolumn]{article}

% ---------------------------------------------------------------------------
% Page geometry
% ---------------------------------------------------------------------------
\RequirePackage[a4paper,margin=0.85in]{geometry}
\setlength{\columnsep}{0.62cm}

% ---------------------------------------------------------------------------
% Type: Libertinus serif/sans/math + Inconsolata mono, shipped in ./fonts/
% ---------------------------------------------------------------------------
\RequirePackage[T1]{fontenc}
\RequirePackage{fontspec}
\RequirePackage{unicode-math}
\defaultfontfeatures{Ligatures=TeX}
\setmainfont{LibertinusSerif-Regular.otf}[
  Path            = fonts/,
  BoldFont        = LibertinusSerif-Bold.otf,
  ItalicFont      = LibertinusSerif-Italic.otf,
  BoldItalicFont  = LibertinusSerif-BoldItalic.otf]
\setsansfont{LibertinusSans-Regular.otf}[
  Path            = fonts/,
  BoldFont        = LibertinusSans-Bold.otf,
  ItalicFont      = LibertinusSans-Italic.otf,
  Scale           = MatchLowercase]
\setmonofont{Inconsolata-Regular.otf}[
  Path            = fonts/,
  BoldFont        = Inconsolata-Bold.otf,
  Scale           = MatchLowercase]
\setmathfont{LibertinusMath-Regular.otf}[Path=fonts/]
\RequirePackage[protrusion=true,expansion=false]{microtype}

% ---------------------------------------------------------------------------
% Layout / tables / lists / links / plots
% ---------------------------------------------------------------------------
\RequirePackage{xcolor}
\RequirePackage{titlesec}
\RequirePackage{titling}
\RequirePackage{booktabs}
\RequirePackage{tabularx}
\RequirePackage{array}
\RequirePackage{enumitem}
\RequirePackage{caption}
\RequirePackage[htt]{hyphenat}       % allow long \texttt{...} tokens to break
\RequirePackage{fancyhdr}
\RequirePackage{graphicx}
\RequirePackage{placeins}
\RequirePackage{dblfloatfix}       % permit full-width figures at page bottoms
\graphicspath{{plots/}}
\RequirePackage[round]{natbib}       % ACL-style author-year citations
\RequirePackage{hyperref}
\RequirePackage{xurl}                % allow long URLs/paths to break
\RequirePackage{etoolbox}

% Compact, ragged-right references: long titles/URLs remain readable without
% creating a mostly empty trailing page.
\AtBeginEnvironment{thebibliography}{%
  \raggedright\footnotesize
  \setlength{\itemsep}{0pt}%
  \setlength{\parsep}{0pt}%
}

% Looser (but tasteful) justification for the narrow two-column measure.
\tolerance=1200
\emergencystretch=2em
% In a ~8.4 cm column, inline author-year citations occasionally leave a line
% with mild interword stretch. Accept badness up to 2500 without a warning (as
% IEEE/ACM two-column classes effectively do); worse underfull lines and all
% overfull boxes are still reported.
\hbadness=2500

% ---------------------------------------------------------------------------
% Palette
% ---------------------------------------------------------------------------
\definecolor{ink}{HTML}{1B1F24}
\definecolor{accent}{HTML}{0B6E4F}      % deep green (compression / go)
\definecolor{accent2}{HTML}{C1543B}     % warm terracotta
\definecolor{slate}{HTML}{51606B}
\definecolor{rule}{HTML}{D8DEE3}
\definecolor{panel}{HTML}{F4F6F7}

\hypersetup{
  colorlinks = true,
  linkcolor  = accent,
  urlcolor   = accent,
  citecolor  = accent,
  pdftitle   = {pigzpp: Fast, Parallel, Portable Compression for the Whole Stack},
  pdfauthor  = {Thamme Gowda}
}

% ---------------------------------------------------------------------------
% Section styling
% ---------------------------------------------------------------------------
\titleformat{\section}
  {\sffamily\large\bfseries\color{ink}\raggedright}{\thesection}{0.55em}{}
\titleformat{\subsection}
  {\sffamily\normalsize\bfseries\color{ink}\raggedright}{\thesubsection}{0.5em}{}
\titlespacing*{\section}{0pt}{1.05em}{0.4em}
\titlespacing*{\subsection}{0pt}{0.7em}{0.25em}

\pagestyle{fancy}
\fancyhf{}
\renewcommand{\headrulewidth}{0pt}
\fancyfoot[C]{\color{slate}\small pigzpp \quad\textbullet\quad
  Fast, Parallel, Portable Compression \quad\textbullet\quad \thepage}

% Don't vertically justify short pages (avoids underfull \vbox warnings from
% full-width figure floats in a two-column layout).
\raggedbottom

\setlist[itemize]{leftmargin=1.25em,itemsep=2pt,topsep=3pt,before=\raggedright}
\newcolumntype{R}{>{\raggedleft\arraybackslash}X}

\captionsetup{font=small,labelfont={bf,color=slate},labelsep=period,
  justification=raggedright,singlelinecheck=true,skip=4pt}

% Favor dense but readable figure placement in the two-column evaluation.
% The defaults are conservative and can leave a half-empty column while a
% queued figure waits for the next page.
\setcounter{topnumber}{4}
\setcounter{bottomnumber}{3}
\setcounter{totalnumber}{6}
\setcounter{dbltopnumber}{2}
\renewcommand{\topfraction}{0.95}
\renewcommand{\bottomfraction}{0.90}
\renewcommand{\textfraction}{0.05}
\renewcommand{\floatpagefraction}{0.82}
\renewcommand{\dbltopfraction}{0.95}
\renewcommand{\dblfloatpagefraction}{0.82}
\setlength{\textfloatsep}{8pt plus 2pt minus 2pt}
\setlength{\floatsep}{7pt plus 2pt minus 2pt}
\setlength{\intextsep}{7pt plus 2pt minus 2pt}
\setlength{\dbltextfloatsep}{9pt plus 2pt minus 2pt}
\setlength{\dblfloatsep}{7pt plus 2pt minus 2pt}

% ---------------------------------------------------------------------------
% Convenience macros for the paper
% ---------------------------------------------------------------------------
\newcommand{\by}{\ensuremath{\times}}          % 7\by  ->  7x
\newcommand{\best}[1]{\textbf{\color{accent}#1}} % highlight a winning number

% Colored key-figure box for the title area
\newcommand{\statbox}[2]{%
  \begin{minipage}[t]{0.31\textwidth}\centering
    {\color{accent}\sffamily\Large\bfseries #1}\\[2pt]
    {\color{slate}\small #2}
  \end{minipage}}

% \titlebanner{title}{subtitle}{author}{url}{tagline} -- full-width masthead
% used inside the \twocolumn[...] optional argument.
\newcommand{\titlebanner}[5]{%
  \begin{center}
    \hypersetup{pageanchor=false}%
    {\color{rule}\rule{\textwidth}{1.1pt}}\\[0.8em]
    {\color{ink}\sffamily\bfseries\fontsize{27}{31}\selectfont #1}\\[0.30em]
    {\color{accent}\sffamily\large\bfseries #2}\\[0.7em]
    {\normalsize #3}\\[0.15em]
    {\small\ttfamily #4}\\[0.35em]
    {\color{slate}\footnotesize #5}\\[0.7em]
    {\color{rule}\rule{\textwidth}{0.5pt}}\\[0.9em]
  \end{center}%
}

\endinput
