#pragma once

// ImportCommitSmoke supplies these in memory; the production preference file
// must never be read or changed by the import transaction fixture.
namespace l2dae {
int lastImportTransitionCurve() noexcept;
bool rememberImportTransitionCurve(int curve) noexcept;
}
