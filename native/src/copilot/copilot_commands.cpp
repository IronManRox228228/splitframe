#include "copilot/copilot_commands.h"
#include "core/json_reader.h"
#include "core/ops.h"

#include <QJsonArray>

namespace sf::copilot {

void registerCopilotCommands(engine::Engine* engine,
                             LintEngine* lintEngine,
                             SuggestionEngine* suggestionEngine,
                             BranchManager* branchManager,
                             ReviewEngine* reviewEngine,
                             index::Catalog* catalog) {
  if (!engine) return;

  // 1. copilot.lint.run
  engine->add(engine::CommandSpec{
    QStringLiteral("copilot.lint.run"),
    QStringLiteral("runEditLint"),
    QStringLiteral("Audits the current timeline against editing and QC rules, returning all active issues."),
    QStringLiteral("copilot"),
    false,
    true,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("gapThresholdMs"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}
      }}
    },
    [lintEngine, catalog](engine::CallContext& ctx, const QJsonObject& args) -> engine::Result {
      if (!ctx.session || !ctx.session->project) {
        return engine::Result::failure(QStringLiteral("no_session"), QStringLiteral("Active project session required"));
      }

      LintOptions opts;
      if (args.contains(QStringLiteral("gapThresholdMs"))) {
        opts.gapThresholdMs = args.value(QStringLiteral("gapThresholdMs")).toVariant().toLongLong();
      }

      const auto issues = lintEngine->analyze(ctx.session->project->doc(), ctx.session->project->assets(), catalog, opts);
      QJsonArray arr;
      for (const auto& iss : issues) arr.append(iss.toJson());

      return engine::Result::success(QJsonObject{
        {QStringLiteral("count"), static_cast<qint64>(issues.size())},
        {QStringLiteral("issues"), arr}
      });
    }
  });

  // 2. copilot.lint.fix
  engine->add(engine::CommandSpec{
    QStringLiteral("copilot.lint.fix"),
    QStringLiteral("fixLintIssue"),
    QStringLiteral("Executes the atomic auto-fix operations for a specific lint issue on the timeline."),
    QStringLiteral("copilot"),
    true,
    true,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("issueId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("issueId")}}
    },
    [lintEngine, catalog](engine::CallContext& ctx, const QJsonObject& args) -> engine::Result {
      if (!ctx.session || !ctx.session->project) {
        return engine::Result::failure(QStringLiteral("no_session"), QStringLiteral("Active project session required"));
      }

      const QString issueId = args.value(QStringLiteral("issueId")).toString();
      if (issueId.isEmpty()) {
        return engine::Result::failure(QStringLiteral("invalid_params"), QStringLiteral("issueId is required"));
      }

      const auto issues = lintEngine->analyze(ctx.session->project->doc(), ctx.session->project->assets(), catalog);
      const LintIssue* target = nullptr;
      for (const auto& iss : issues) {
        if (iss.id == issueId) {
          target = &iss;
          break;
        }
      }

      if (!target) {
        return engine::Result::failure(QStringLiteral("issue_not_found"), QStringLiteral("No issue with id '%1'").arg(issueId));
      }
      if (!target->autoFixAvailable || target->fixOps.isEmpty()) {
        return engine::Result::failure(QStringLiteral("no_fix_available"), QStringLiteral("Issue '%1' does not have an automatic fix").arg(issueId));
      }

      std::vector<Op> ops;
      for (const auto& opJson : target->fixOps) {
        try {
          ops.push_back(parseOp(Rd(QJsonValue(opJson))));
        } catch (const std::exception& e) {
          return engine::Result::failure(QStringLiteral("op_parse_error"), QString::fromUtf8(e.what()));
        }
      }

      const bool ok = ctx.session->project->apply(ops, QStringLiteral("Fix %1").arg(target->ruleId), ctx.actor());
      if (!ok) {
        return engine::Result::failure(QStringLiteral("apply_failed"), ctx.session->project->lastError());
      }

      return engine::Result::success(QJsonObject{
        {QStringLiteral("fixed"), true},
        {QStringLiteral("ruleId"), target->ruleId},
        {QStringLiteral("opsApplied"), static_cast<qint64>(ops.size())}
      });
    }
  });

  // 3. copilot.suggest.get
  engine->add(engine::CommandSpec{
    QStringLiteral("copilot.suggest.get"),
    QStringLiteral("getSuggestion"),
    QStringLiteral("Returns the currently active next-edit ghost suggestion, if any."),
    QStringLiteral("copilot"),
    false,
    false,
    QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    [suggestionEngine](engine::CallContext&, const QJsonObject&) -> engine::Result {
      const auto s = suggestionEngine->currentSuggestion();
      if (!s) {
        return engine::Result::success(QJsonObject{{QStringLiteral("hasSuggestion"), false}});
      }
      QJsonObject res = s->toJson();
      res.insert(QStringLiteral("hasSuggestion"), true);
      return engine::Result::success(res);
    }
  });

  // 4. copilot.suggest.accept
  engine->add(engine::CommandSpec{
    QStringLiteral("copilot.suggest.accept"),
    QStringLiteral("acceptSuggestion"),
    QStringLiteral("Accepts and commits the active next-edit suggestion to the timeline."),
    QStringLiteral("copilot"),
    true,
    true,
    QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    [suggestionEngine](engine::CallContext& ctx, const QJsonObject&) -> engine::Result {
      if (!ctx.session || !ctx.session->project) {
        return engine::Result::failure(QStringLiteral("no_session"), QStringLiteral("Active project session required"));
      }

      const auto s = suggestionEngine->currentSuggestion();
      if (!s) {
        return engine::Result::failure(QStringLiteral("no_suggestion"), QStringLiteral("No active suggestion to accept"));
      }

      const bool ok = suggestionEngine->accept(ctx.session->project);
      if (!ok) {
        return engine::Result::failure(QStringLiteral("accept_failed"), ctx.session->project->lastError());
      }

      return engine::Result::success(QJsonObject{
        {QStringLiteral("accepted"), true},
        {QStringLiteral("title"), s->title}
      });
    }
  });

  // 5. copilot.suggest.dismiss
  engine->add(engine::CommandSpec{
    QStringLiteral("copilot.suggest.dismiss"),
    QStringLiteral("dismissSuggestion"),
    QStringLiteral("Dismisses the current ghost suggestion and emits dismissal notification."),
    QStringLiteral("copilot"),
    false, // document-read-only, but state-modifying on suggestion slot
    false,
    QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    [engine, suggestionEngine](engine::CallContext&, const QJsonObject&) -> engine::Result {
      suggestionEngine->dismiss();
      engine->emitEvent(QStringLiteral("copilot.suggestion.dismissed"), QJsonObject{});
      return engine::Result::success(QJsonObject{{QStringLiteral("dismissed"), true}});
    }
  });

  // 6. copilot.review.run
  engine->add(engine::CommandSpec{
    QStringLiteral("copilot.review.run"),
    QStringLiteral("reviewTimeline"),
    QStringLiteral("Executes an autonomous self-review pass on the current timeline, checking technical and narrative continuity. With applySafeFixes=true the collected safe fixes are applied for real, all-or-nothing in one undo step, and the response reports autoFixApplied."),
    QStringLiteral("copilot"),
    true,
    true,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("applySafeFixes"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
        {QStringLiteral("passThreshold"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}
      }}
    },
    [reviewEngine, catalog](engine::CallContext& ctx, const QJsonObject& args) -> engine::Result {
      if (!ctx.session || !ctx.session->project) {
        return engine::Result::failure(QStringLiteral("no_session"), QStringLiteral("Active project session required"));
      }

      ReviewOptions opts;
      if (args.contains(QStringLiteral("applySafeFixes"))) opts.applySafeFixes = args.value(QStringLiteral("applySafeFixes")).toBool();
      if (args.contains(QStringLiteral("passThreshold"))) opts.passThreshold = args.value(QStringLiteral("passThreshold")).toDouble(0.70);

      auto res = reviewEngine->reviewTimeline(ctx.session->project->doc(), ctx.session->project->assets(), catalog, opts);
      if (opts.applySafeFixes && !res.autoFixOps.isEmpty()) {
        // The safe fixes only exist as ops above; apply them here for real, all-or-nothing
        // in one undo group: a failure rolls the whole autofix back and reports false.
        editor::Project& project = *ctx.session->project;
        project.beginGroup(QStringLiteral("Review safe fixes"), ctx.actor());
        bool applied = true;
        for (const auto& opJson : res.autoFixOps) {
          try {
            const std::vector<Op> ops{parseOp(Rd(QJsonValue(opJson)))};
            if (!project.apply(ops, QStringLiteral("Review safe fix"))) {
              applied = false;
              break;
            }
          } catch (...) {
            applied = false;
            break;
          }
        }
        if (applied) project.endGroup();
        else project.rollbackGroup();
        res.autoFixApplied = applied;
      }
      return engine::Result::success(res.toJson());
    }
  });

  // 7. branch.create
  engine->add(engine::CommandSpec{
    QStringLiteral("branch.create"),
    QStringLiteral("createBranch"),
    QStringLiteral("Creates a new timeline branch from the active timeline or specified base."),
    QStringLiteral("branch"),
    true,
    true,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("name"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("author"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("base"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}}
    },
    [branchManager](engine::CallContext& ctx, const QJsonObject& args) -> engine::Result {
      if (!branchManager) return engine::Result::failure(QStringLiteral("no_manager"), QStringLiteral("BranchManager not initialized"));
      if (ctx.session && ctx.session->project) {
        branchManager->attachProject(ctx.session->project);
      }

      const QString name = args.value(QStringLiteral("name")).toString();
      const QString author = args.value(QStringLiteral("author")).toString(ctx.actor());
      const QString base = args.value(QStringLiteral("base")).toString();

      const bool ok = branchManager->createBranch(name, author, base);
      if (!ok) {
        return engine::Result::failure(QStringLiteral("branch_create_failed"), QStringLiteral("Could not create branch '%1' (invalid or already exists)").arg(name));
      }

      return engine::Result::success(QJsonObject{
        {QStringLiteral("created"), true},
        {QStringLiteral("name"), name},
        {QStringLiteral("active"), branchManager->activeBranch()}
      });
    }
  });

  // 8. branch.switch
  engine->add(engine::CommandSpec{
    QStringLiteral("branch.switch"),
    QStringLiteral("switchBranch"),
    QStringLiteral("Switches the active timeline to the specified branch."),
    QStringLiteral("branch"),
    true,
    true,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("name"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}}
    },
    [branchManager](engine::CallContext& ctx, const QJsonObject& args) -> engine::Result {
      if (!branchManager) return engine::Result::failure(QStringLiteral("no_manager"), QStringLiteral("BranchManager not initialized"));
      if (ctx.session && ctx.session->project) {
        branchManager->attachProject(ctx.session->project);
      }

      const QString name = args.value(QStringLiteral("name")).toString();
      const bool ok = branchManager->switchBranch(name);
      if (!ok) {
        return engine::Result::failure(QStringLiteral("branch_not_found"), QStringLiteral("Branch '%1' does not exist").arg(name));
      }

      return engine::Result::success(QJsonObject{
        {QStringLiteral("switched"), true},
        {QStringLiteral("active"), branchManager->activeBranch()}
      });
    }
  });

  // 9. branch.list
  engine->add(engine::CommandSpec{
    QStringLiteral("branch.list"),
    QStringLiteral("listBranches"),
    QStringLiteral("Lists all timeline branches and their metadata."),
    QStringLiteral("branch"),
    false,
    false,
    QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
    [branchManager](engine::CallContext&, const QJsonObject&) -> engine::Result {
      if (!branchManager) return engine::Result::failure(QStringLiteral("no_manager"), QStringLiteral("BranchManager not initialized"));

      const auto list = branchManager->listBranches();
      QJsonArray arr;
      for (const auto& b : list) {
        arr.append(QJsonObject{
          {QStringLiteral("id"), b.id},
          {QStringLiteral("name"), b.name},
          {QStringLiteral("author"), b.author},
          {QStringLiteral("baseBranch"), b.baseBranch},
          {QStringLiteral("createdAt"), b.createdAt},
          {QStringLiteral("updatedAt"), b.updatedAt},
          {QStringLiteral("itemCount"), static_cast<qint64>(b.doc.items.size())},
          {QStringLiteral("isActive"), (b.name == branchManager->activeBranch())}
        });
      }

      return engine::Result::success(QJsonObject{
        {QStringLiteral("active"), branchManager->activeBranch()},
        {QStringLiteral("branches"), arr}
      });
    }
  });

  // 10. branch.diff
  engine->add(engine::CommandSpec{
    QStringLiteral("branch.diff"),
    QStringLiteral("diffBranches"),
    QStringLiteral("Computes a structured diff between two timeline branches."),
    QStringLiteral("branch"),
    false,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("nameA"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("nameB"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("nameA"), QStringLiteral("nameB")}}
    },
    [branchManager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      if (!branchManager) return engine::Result::failure(QStringLiteral("no_manager"), QStringLiteral("BranchManager not initialized"));

      const QString nameA = args.value(QStringLiteral("nameA")).toString();
      const QString nameB = args.value(QStringLiteral("nameB")).toString();

      const auto diff = branchManager->diffBranches(nameA, nameB);
      return engine::Result::success(diff.toJson());
    }
  });

  // 11. branch.merge
  engine->add(engine::CommandSpec{
    QStringLiteral("branch.merge"),
    QStringLiteral("mergeBranch"),
    QStringLiteral("Merges changes from a source branch into the active branch. The active branch's items and tracks are made equal to the source branch's (no base-document three-way merge); `strategy` is accepted but only the default behaviour exists."),
    QStringLiteral("branch"),
    true,
    true,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("sourceBranch"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("strategy"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("sourceBranch")}}
    },
    [branchManager](engine::CallContext& ctx, const QJsonObject& args) -> engine::Result {
      if (!branchManager) return engine::Result::failure(QStringLiteral("no_manager"), QStringLiteral("BranchManager not initialized"));
      if (ctx.session && ctx.session->project) {
        branchManager->attachProject(ctx.session->project);
      }

      const QString src = args.value(QStringLiteral("sourceBranch")).toString();
      const QString strat = args.value(QStringLiteral("strategy")).toString(QStringLiteral("all"));

      const bool ok = branchManager->mergeBranch(src, strat);
      if (!ok) {
        return engine::Result::failure(QStringLiteral("merge_failed"), QStringLiteral("Could not merge branch '%1' into '%2'").arg(src, branchManager->activeBranch()));
      }

      return engine::Result::success(QJsonObject{
        {QStringLiteral("merged"), true},
        {QStringLiteral("sourceBranch"), src},
        {QStringLiteral("activeBranch"), branchManager->activeBranch()}
      });
    }
  });

  // 12. branch.delete
  engine->add(engine::CommandSpec{
    QStringLiteral("branch.delete"),
    QStringLiteral("deleteBranch"),
    QStringLiteral("Deletes an inactive timeline branch."),
    QStringLiteral("branch"),
    true,
    false,
    QJsonObject{
      {QStringLiteral("type"), QStringLiteral("object")},
      {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("name"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
      }},
      {QStringLiteral("required"), QJsonArray{QStringLiteral("name")}}
    },
    [branchManager](engine::CallContext&, const QJsonObject& args) -> engine::Result {
      if (!branchManager) return engine::Result::failure(QStringLiteral("no_manager"), QStringLiteral("BranchManager not initialized"));

      const QString name = args.value(QStringLiteral("name")).toString();
      const bool ok = branchManager->deleteBranch(name);
      if (!ok) {
        return engine::Result::failure(QStringLiteral("delete_failed"), QStringLiteral("Cannot delete branch '%1' (active, 'main', or does not exist)").arg(name));
      }

      return engine::Result::success(QJsonObject{
        {QStringLiteral("deleted"), true},
        {QStringLiteral("name"), name}
      });
    }
  });
}

} // namespace sf::copilot
