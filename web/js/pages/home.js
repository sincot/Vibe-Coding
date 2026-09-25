import { getUser, isLoggedIn } from "../auth.js";
import { h } from "../util.js";

const SVG_NS = "http://www.w3.org/2000/svg";

function icon(paths, extraClass = "") {
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.8");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("focusable", "false");
  svg.setAttribute("class", "home-icon" + (extraClass ? " " + extraClass : ""));
  for (const d of [].concat(paths)) {
    const path = document.createElementNS(SVG_NS, "path");
    path.setAttribute("d", d);
    svg.appendChild(path);
  }
  return svg;
}

const ICONS = {
  terminal: ["M4 17l6-5-6-5", "M12 19h8"],
  zap: ["M13 2L3 14h9l-1 8 10-12h-9l1-8z"],
  filter: ["M4 6h16", "M7 12h10", "M10 18h4"],
  trophy: [
    "M8 21h8",
    "M12 17v4",
    "M7 4h10v5a5 5 0 0 1-10 0V4z",
    "M5 4H3v2a3 3 0 0 0 3 3",
    "M19 4h2v2a3 3 0 0 1-3 3",
  ],
  history: ["M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z", "M12 8v4l3 2"],
  shield: ["M12 3l7 3v5c0 4.5-3 8-7 10-4-2-7-5.5-7-10V6l7-3z", "M9 12l2 2 4-4"],
  check: ["M22 11.08V12a10 10 0 1 1-5.93-9.14", "M22 4L12 14l-3-3"],
  arrow: ["M5 12h14", "M13 6l6 6-6 6"],
  play: ["M6 4l14 8-14 8V4z"],
  code: ["M16 18l6-6-6-6", "M8 6l-6 6 6 6"],
  users: [
    "M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2",
    "M9 11a4 4 0 1 0 0-8 4 4 0 0 0 0 8z",
    "M23 21v-2a4 4 0 0 0-3-3.87",
    "M16 3.13a4 4 0 0 1 0 7.75",
  ],
  target: ["M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z", "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8z", "M12 12h.01"],
  layers: ["M12 3l9 5-9 5-9-5 9-5z", "M3 13l9 5 9-5", "M3 17l9 5 9-5"],
  clock: ["M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z", "M12 7v5l3 2"],
};

function actionLink(label, href, variant, iconPaths) {
  return h(
    "a",
    { class: `btn ${variant} home-cta`, attrs: { href: "#" + href } },
    [h("span", { text: label }), icon(iconPaths)]
  );
}

function featureCard(iconName, title, desc) {
  return h("article", { class: "card home-feature" }, [
    h("span", { class: "home-feature-icon" }, [icon(ICONS[iconName])]),
    h("h3", { text: title }),
    h("p", { class: "muted", text: desc }),
  ]);
}

function stepCard(index, title, desc) {
  return h("li", { class: "home-step" }, [
    h("span", { class: "home-step-index", text: String(index) }),
    h("div", {}, [
      h("h3", { text: title }),
      h("p", { class: "muted", text: desc }),
    ]),
  ]);
}

function statItem(value, label) {
  return h("div", { class: "home-stat" }, [
    h("span", { class: "home-stat-value", text: value }),
    h("span", { class: "home-stat-label", text: label }),
  ]);
}

function faqItem(question, answer) {
  return h("details", { class: "home-faq-item" }, [
    h("summary", {}, [h("span", { text: question }), icon(ICONS.arrow)]),
    h("p", { class: "muted", text: answer }),
  ]);
}

function hero() {
  const loggedIn = isLoggedIn();
  const user = getUser();
  const nickname = user && user.nickname ? user.nickname : "同学";

  const actions = loggedIn
    ? [
        actionLink("进入题目界面", "/problems", "btn-primary", ICONS.arrow),
        actionLink("查看排行榜", "/leaderboard", "btn-secondary", ICONS.trophy),
      ]
    : [
        actionLink("进入系统", "/login", "btn-primary", ICONS.arrow),
        actionLink("注册新账号", "/register", "btn-secondary", ICONS.play),
      ];

  const consoleCard = h(
    "div",
    { class: "home-console", attrs: { "aria-hidden": "true" } },
    [
      h("div", { class: "home-console-bar" }, [
        h("span", { class: "home-dot" }),
        h("span", { class: "home-dot" }),
        h("span", { class: "home-dot" }),
        h("span", { class: "home-console-title", text: "solution.cpp" }),
      ]),
      h("pre", {
        class: "home-code",
        text: [
          "#include <iostream>",
          "using namespace std;",
          "",
          "int main() {",
          "    long long a, b;",
          "    cin >> a >> b;",
          "    cout << a + b << endl;",
          "    return 0;",
          "}",
        ].join("\n"),
      }),
      h("div", { class: "home-verdicts" }, [
        h("span", { class: "badge status-AC", text: "AC" }),
        h("span", { class: "home-verdict-text", text: "通过全部测试点 · 12ms · 1.8MB" }),
      ]),
    ]
  );

  const greeting = loggedIn
    ? h("p", { class: "home-hero-note" }, [
        h("span", { text: `欢迎回来，${nickname}。` }),
        h("a", { text: "查看我的提交记录", attrs: { href: "#/submissions" } }),
      ])
    : h("p", { class: "home-hero-note" }, [
        h("span", { text: "首次使用请先" }),
        h("a", { text: "注册账号", attrs: { href: "#/register" } }),
        h("span", { text: "，已有账号可" }),
        h("a", { text: "直接登录", attrs: { href: "#/login" } }),
        h("span", { text: "。登录后即可浏览题库并提交代码。" }),
      ]);

  return h("section", { class: "home-hero" }, [
    h("div", { class: "home-container home-hero-inner" }, [
      h("div", { class: "home-hero-copy" }, [
        h("span", { class: "home-eyebrow", text: "教学用 · 在线判题系统" }),
        h("h1", { class: "home-headline", text: "把每一次提交，都变成看得见的进步" }),
        h("p", {
          class: "home-lede",
          text: "支持 C++17 与 C11 在线提交与自动评测，提供题库浏览、实时判题结果、提交历史与排行榜。无需本地环境，打开浏览器即可开始练习。",
        }),
        h("div", { class: "home-actions" }, actions),
        greeting,
      ]),
      h("div", { class: "home-hero-visual" }, [consoleCard]),
    ]),
  ]);
}

function stats() {
  return h("section", { class: "home-stats-band" }, [
    h("div", { class: "home-container home-stats" }, [
      statItem("C++17 / C11", "在线编译语言"),
      statItem("自动评测", "提交即编译运行"),
      statItem("逐点判定", "AC / WA / TLE / MLE"),
      statItem("公开排行", "按 AC 与提交排名"),
    ]),
  ]);
}

function features() {
  return h("section", { class: "home-section" }, [
    h("div", { class: "home-container" }, [
      h("h2", { class: "home-section-title", text: "为什么选择 OJ 在线判题" }),
      h("p", {
        class: "home-section-subtitle muted",
        text: "从浏览题目到拿到判定结果，把学习闭环压缩在同一个网页里。",
      }),
      h("div", { class: "home-feature-grid" }, [
        featureCard(
          "terminal",
          "在线编译评测",
          "提交 C++17 / C11 代码，后端自动编译运行，无需配置任何本地环境。"
        ),
        featureCard(
          "zap",
          "实时判题结果",
          "返回 AC、WA、TLE、MLE 等判定，并附耗时、内存与编译错误信息。"
        ),
        featureCard(
          "filter",
          "筛选与标签",
          "按关键词、难度与标签快速定位题目，并记录本人 AC 状态。"
        ),
        featureCard(
          "trophy",
          "公开排行榜",
          "按通过题数与提交表现排名，让进步和同伴的节奏都清晰可见。"
        ),
        featureCard(
          "history",
          "提交历史",
          "完整保存每次提交的代码与判定结果，方便复盘与对比改进。"
        ),
        featureCard(
          "shield",
          "教学与管理",
          "管理员可维护题目、测试点与用户，并支持重新评测，保障教学秩序。"
        ),
      ]),
    ]),
  ]);
}

function workflow() {
  return h("section", { class: "home-section home-section-alt" }, [
    h("div", { class: "home-container" }, [
      h("h2", { class: "home-section-title", text: "三步开始练习" }),
      h("p", {
        class: "home-section-subtitle muted",
        text: "不需要安装任何软件，注册并登录后即可走完一次完整的评测流程。",
      }),
      h("ol", { class: "home-steps" }, [
        stepCard(1, "注册并登录", "注册后获得系统分配的账号，使用账号登录进入题库。"),
        stepCard(2, "选择题目并提交", "按难度和标签挑题，在内置编辑器中编写代码并提交。"),
        stepCard(3, "查看判定结果", "等待评测完成，查看每个测试点的状态与通过情况。"),
      ]),
    ]),
  ]);
}

function languages() {
  const card = (name, mode, code) =>
    h("article", { class: "card home-language" }, [
      h("div", { class: "home-language-head" }, [
        h("span", { class: "home-language-icon" }, [icon(ICONS.code)]),
        h("div", {}, [
          h("h3", { text: name }),
          h("p", { class: "muted", text: mode }),
        ]),
      ]),
      h("pre", { class: "home-language-code", text: code }),
    ]);

  return h("section", { class: "home-section" }, [
    h("div", { class: "home-container" }, [
      h("h2", { class: "home-section-title", text: "支持的提交语言" }),
      h("p", {
        class: "home-section-subtitle muted",
        text: "选择语言后在内置编辑器中编写代码，一键提交即可开始评测。",
      }),
      h("div", { class: "home-language-grid" }, [
        card(
          "C++17",
          "标准 C++17（含 STL）",
          "#include <iostream>\nusing namespace std;\n\nint main() {\n    long long a, b;\n    cin >> a >> b;\n    cout << a + b << '\\n';\n    return 0;\n}"
        ),
        card(
          "C11",
          "标准 C11",
          "#include <stdio.h>\n\nint main(void) {\n    long long a, b;\n    if (scanf(\"%lld %lld\", &a, &b) != 2) return 0;\n    printf(\"%lld\\n\", a + b);\n    return 0;\n}"
        ),
      ]),
    ]),
  ]);
}

function audiences() {
  const card = (iconName, title, desc) =>
    h("article", { class: "card home-audience" }, [
      h("span", { class: "home-audience-icon" }, [icon(ICONS[iconName])]),
      h("h3", { text: title }),
      h("p", { class: "muted", text: desc }),
    ]);

  return h("section", { class: "home-section home-section-alt" }, [
    h("div", { class: "home-container" }, [
      h("h2", { class: "home-section-title", text: "适合谁使用" }),
      h("p", {
        class: "home-section-subtitle muted",
        text: "无论是课堂练习还是自学刷题，都可以在同一个系统里完成。",
      }),
      h("div", { class: "home-audience-grid" }, [
        card("users", "学生", "按班级或课程练题，随时查看自己的 AC 进度与排名。"),
        card("target", "教师", "发布与维护题目、组织练习，用排行榜了解整体情况。"),
        card("clock", "自学者", "利用碎片时间练习，提交历史帮助持续复盘与提升。"),
      ]),
    ]),
  ]);
}

function faq() {
  return h("section", { class: "home-section" }, [
    h("div", { class: "home-container home-faq" }, [
      h("h2", { class: "home-section-title", text: "常见问题" }),
      h("div", { class: "home-faq-list" }, [
        faqItem("需要安装本地环境吗？", "不需要。所有编译与运行都在服务器端完成，你只需要浏览器和网络。"),
        faqItem("支持哪些语言？", "当前支持 C++17 与 C11，提交时可在编辑器中切换。"),
        faqItem("为什么必须先登录？", "题目、提交与排名都与账号关联，登录后才能记录你的做题进度。"),
        faqItem("判定结果有哪些？", "包括 AC（通过）、WA、TLE、MLE、CE、RE 等，并会展示逐测试点详情。"),
        faqItem("管理员首次登录要注意什么？", "管理员首次登录需按提示修改初始密码，完成后即可进入后台。"),
      ]),
    ]),
  ]);
}

function ctaBand() {
  const loggedIn = isLoggedIn();
  const primary = loggedIn
    ? actionLink("进入题目界面", "/problems", "btn-primary", ICONS.arrow)
    : actionLink("进入系统", "/login", "btn-primary", ICONS.arrow);

  return h("section", { class: "home-section" }, [
    h("div", { class: "home-container" }, [
      h("div", { class: "home-cta-band" }, [
        h("div", {}, [
          h("h2", { text: "准备好写下第一行代码了吗？" }),
          h("p", { class: "muted", text: "现在进入系统，挑一道最简单的题目开始吧。" }),
        ]),
        primary,
      ]),
    ]),
  ]);
}

export function renderHome(container) {
  document.title = "OJ 在线判题系统";
  container.replaceChildren();
  container.classList.add("home");

  container.appendChild(hero());
  container.appendChild(stats());
  container.appendChild(features());
  container.appendChild(workflow());
  container.appendChild(languages());
  container.appendChild(audiences());
  container.appendChild(faq());
  container.appendChild(ctaBand());

  return () => {
    container.classList.remove("home");
  };
}
